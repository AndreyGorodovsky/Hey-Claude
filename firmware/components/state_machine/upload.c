/*
 * The request upload: one task that, for each request, tells the server it
 * has begun and then reads the microphone's audio from the audio ring and
 * sends it in 20 ms frames.
 *
 * Everything the device sends to the server during a turn is sent from
 * here. Sending can block for seconds when the network stalls, and the
 * state machine must never wait that long, so it hands the whole of it to
 * this task.
 *
 * The audio starts from the position the state machine names: the moment
 * the chime ended, which is slightly in the past by the time this
 * task has woken and announced the request. The ring still holds that
 * audio, so the first reads return at once and those frames go out in a
 * burst; after that each read waits for 20 ms of new audio, and the upload
 * runs in step with the microphone.
 *
 * The task runs on core 0, with the network stack it feeds, at a priority
 * above the display's so that drawing cannot delay the request
 * (ARCHITECTURE.md, task table). Reading the ring is safe from any core.
 *
 * A "run" number ties the task to one request. upload_start() raises it and
 * upload_stop() raises it again; the task sends only while the number is
 * the one it started with. Stopping is therefore just a number changing: it
 * needs no waiting, and a stop followed at once by a new start cannot leave
 * the task sending the old request.
 */
#include "upload.h"

#include <stdatomic.h>
#include <stdbool.h>
#include "audio_ring.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "server_link.h"
#include "protocol.h"

static const char *TAG = "upload";

/* Samples in one of the protocol's frames: 320, for 20 ms at 16 kHz */
#define FRAME_SAMPLES       (PROTO_CAPTURE_RATE_HZ * PROTO_CAPTURE_FRAME_MS / 1000)
/* Longest wait for one frame of audio from the ring. The microphone
 * delivers a frame every 20 ms; this long without one means it has stopped. */
#define READ_TIMEOUT_MS     500
/* Sends that may fail in a row before the upload is given up. A failed
 * send means the connection has ended or is ending (server_link.h); the few
 * retries only cover the moment in which that becomes known. */
#define SEND_FAILURES_MAX   3

/* In bytes. More than the task's own work needs: when a send fails, the
 * WebSocket client reports the failure from inside the send, so its error
 * handling and logging run on this stack too (server_link.c). */
#define TASK_STACK          6144
#define TASK_PRIO           6
#define NETWORK_CORE        0

static TaskHandle_t s_task;
static void (*s_on_failed)(upload_failure_t why);
/* Even while sending, odd while stopped. atomic_uint can be read and
 * written from several tasks without a lock. */
static atomic_uint s_run;
/* Where the next request starts. Written by upload_start() before it raises
 * s_run, and read by the task only after it has seen the new number. */
static uint32_t s_start_pos;

void upload_start(uint32_t pos)
{
    s_start_pos = pos;
    /* To the next even number, whatever it was */
    atomic_store(&s_run, (atomic_load(&s_run) | 1u) + 1u);
    xTaskNotifyGive(s_task);
}

void upload_stop(void)
{
    atomic_fetch_or(&s_run, 1u);
}

/* Reports a failure, unless the request was stopped meanwhile: a stop that
 * arrived while a send was failing makes the failure irrelevant */
static void failed(unsigned run, upload_failure_t why)
{
    if (atomic_load(&s_run) == run) {
        s_on_failed(why);
    }
}

/* Sends one request. Returns when it is stopped or cannot go on. */
static void send_request(unsigned run, uint32_t start_pos)
{
    /* static: 640 bytes kept off the task's stack; only this task uses it */
    static int16_t frame[FRAME_SAMPLES];

    if (server_link_send_utterance_start() != ESP_OK) {
        ESP_LOGE(TAG, "the request could not be announced to the server");
        failed(run, UPLOAD_FAIL_SEND);
        return;
    }
    audio_ring_reader_t reader;
    audio_ring_reader_init_at(&reader, start_pos);
    /* How far behind the microphone the upload begins: the audio already
     * captured since the request's starting position, which goes out in
     * the opening burst */
    ESP_LOGI(TAG, "request begins %lu ms behind the microphone",
             (unsigned long)((audio_ring_now() - reader.pos) * 1000u / PROTO_CAPTURE_RATE_HZ));

    uint32_t frames = 0;
    uint32_t dropped = 0;
    int failures = 0;
    while (atomic_load(&s_run) == run) {
        esp_err_t err = audio_ring_read(&reader, frame, FRAME_SAMPLES, READ_TIMEOUT_MS,
                                        &dropped);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "no audio from the microphone: %s", esp_err_to_name(err));
            failed(run, UPLOAD_FAIL_MICROPHONE);
            break;
        }
        if (server_link_send_audio(frame, FRAME_SAMPLES) == ESP_OK) {
            failures = 0;
            frames++;
        } else if (++failures >= SEND_FAILURES_MAX) {
            ESP_LOGE(TAG, "request audio is not getting through");
            failed(run, UPLOAD_FAIL_SEND);
            break;
        }
    }
    /* `dropped` counts samples the ring had already overwritten because
     * this task fell more than 2 s behind, as it can when a send stalls */
    ESP_LOGI(TAG, "sent %lu frames (%lu ms); %lu ms of audio lost on the way",
             (unsigned long)frames, (unsigned long)(frames * PROTO_CAPTURE_FRAME_MS),
             (unsigned long)(dropped * 1000u / PROTO_CAPTURE_RATE_HZ));
}

static void upload_task(void *arg)
{
    for (;;) {
        /* Sleeps until a request starts */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        unsigned run = atomic_load(&s_run);
        if (run & 1u) {
            continue;   /* stopped again before this task woke */
        }
        send_request(run, s_start_pos);
    }
}

esp_err_t upload_init(void (*on_failed)(upload_failure_t why))
{
    s_on_failed = on_failed;
    atomic_store(&s_run, 1u);   /* stopped */
    BaseType_t made = xTaskCreatePinnedToCore(upload_task, "upload", TASK_STACK, NULL,
                                              TASK_PRIO, &s_task, NETWORK_CORE);
    return made == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
