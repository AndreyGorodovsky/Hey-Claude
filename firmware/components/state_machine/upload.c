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
 *
 * `cancel`, sent when the person presses the button, goes from here too,
 * for two reasons. Sending can block, as above. And it must follow the
 * request's `utterance_start` and audio on the connection, never overtake
 * them, which one task sending everything in turn guarantees. The run
 * number also tells whether the request being cancelled was ever announced:
 * one stopped before this task woke to announce it has no turn on the
 * server, and its `cancel` would never be answered.
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
static void (*s_on_report)(upload_report_t what);
/* Even while sending, odd while stopped. atomic_uint can be read and
 * written from several tasks without a lock. */
static atomic_uint s_run;
/* Where the next request starts, and the volume it states. Written by
 * upload_start() before it raises s_run, and read by the task only after it
 * has seen the new number. */
static uint32_t s_start_pos;
static uint8_t s_start_volume;
/* A cancel asked for, and the run of the request it is for. The run is
 * written by upload_cancel() before it sets the flag, and read by the task
 * only after it has taken the flag. */
static atomic_bool s_cancel_asked;
static unsigned s_cancel_run;
/* Used only by the task: the run whose `utterance_start` was last sent. 0,
 * which is never a run's number, until the first. */
static unsigned s_announced_run;

void upload_start(uint32_t pos, uint8_t volume)
{
    s_start_pos = pos;
    s_start_volume = volume;
    /* To the next even number, whatever it was */
    atomic_store(&s_run, (atomic_load(&s_run) | 1u) + 1u);
    xTaskNotifyGive(s_task);
}

void upload_stop(void)
{
    atomic_fetch_or(&s_run, 1u);
}

void upload_cancel(void)
{
    /* The latest request's run, whether it is still sending (an even
     * number) or was stopped already (the odd number after it) */
    s_cancel_run = atomic_load(&s_run) & ~1u;
    atomic_store(&s_cancel_asked, true);
    upload_stop();
    xTaskNotifyGive(s_task);
}

/* Reports a failure, unless the request was stopped meanwhile: a stop that
 * arrived while a send was failing makes the failure irrelevant */
static void failed(unsigned run, upload_report_t why)
{
    if (atomic_load(&s_run) == run) {
        s_on_report(why);
    }
}

/* Sends the cancel asked for by upload_cancel(), if its request was ever
 * announced to the server, and reports how that went */
static void send_cancel(void)
{
    if (s_announced_run != s_cancel_run) {
        ESP_LOGI(TAG, "cancel not sent: the request was never announced");
        s_on_report(UPLOAD_CANCEL_NOT_NEEDED);
        return;
    }
    if (server_link_send_cancel() != ESP_OK) {
        /* A failed send ends the connection (server_link.h), which ends the
         * turn on the server as surely as `cancel` would */
        ESP_LOGW(TAG, "cancel could not be sent");
        s_on_report(UPLOAD_CANCEL_FAILED);
        return;
    }
    ESP_LOGI(TAG, "cancel sent");
    s_on_report(UPLOAD_CANCEL_SENT);
}

/* Sends one request. Returns when it is stopped or cannot go on. */
static void send_request(unsigned run, uint32_t start_pos, uint8_t volume)
{
    /* static: 640 bytes kept off the task's stack; only this task uses it */
    static int16_t frame[FRAME_SAMPLES];

    if (server_link_send_utterance_start(volume) != ESP_OK) {
        ESP_LOGE(TAG, "the request could not be announced to the server");
        failed(run, UPLOAD_FAIL_SEND);
        return;
    }
    s_announced_run = run;
    audio_ring_reader_t reader;
    audio_ring_reader_init_at(&reader, start_pos);
    if (reader.pos != start_pos) {
        /* The ring no longer offers the audio the request starts with, and
         * has put the reader at the oldest it does offer. That happens
         * when announcing the request, just above, stalled for about a
         * second or more. The request would begin part-way through its
         * first words, so it is given up, like one with a hole further in
         * (below). */
        ESP_LOGE(TAG, "the start of the request was lost before it could be sent");
        failed(run, UPLOAD_FAIL_SEND);
        return;
    }
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
        if (dropped > 0) {
            /* The ring has overwritten audio this task had not yet read,
             * which takes falling 2 s behind the microphone, as a send
             * stalled on a network that has stopped moving can cause. The
             * request now has a hole in it. It is given up, like a reply
             * with a hole (player.c): a request missing some of its words
             * would be answered as if it were whole. */
            ESP_LOGE(TAG, "request audio was lost before it could be sent");
            failed(run, UPLOAD_FAIL_SEND);
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
    /* `dropped` counts the samples lost in the case above */
    ESP_LOGI(TAG, "sent %lu frames (%lu ms); %lu ms of audio lost on the way",
             (unsigned long)frames, (unsigned long)(frames * PROTO_CAPTURE_FRAME_MS),
             (unsigned long)(dropped * 1000u / PROTO_CAPTURE_RATE_HZ));
}

static void upload_task(void *arg)
{
    for (;;) {
        /* Sleeps until a request starts or a cancel is asked for */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        unsigned run = atomic_load(&s_run);
        /* An odd number: stopped again before this task woke, or woken
         * only for a cancel */
        if (!(run & 1u)) {
            send_request(run, s_start_pos, s_start_volume);
        }
        /* After the request has stopped sending, so that the cancel follows
         * the last of its audio. A cancel asked for while a request was
         * sending is what stopped it. */
        if (atomic_exchange(&s_cancel_asked, false)) {
            send_cancel();
        }
    }
}

esp_err_t upload_init(void (*on_report)(upload_report_t what))
{
    s_on_report = on_report;
    atomic_store(&s_run, 1u);   /* stopped */
    BaseType_t made = xTaskCreatePinnedToCore(upload_task, "upload", TASK_STACK, NULL,
                                              TASK_PRIO, &s_task, NETWORK_CORE);
    return made == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
