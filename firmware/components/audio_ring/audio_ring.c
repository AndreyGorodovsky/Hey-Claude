/*
 * The microphone's audio, shared through a ring buffer. See audio_ring.h.
 *
 * A ring buffer is a fixed block of memory used round and round: the writer
 * fills it from the start, and on reaching the end carries on from the start
 * again, overwriting the oldest audio. Nothing is ever moved or freed; only
 * a position advances. Here the block holds AUDIO_RING_SAMPLES samples and
 * lives in PSRAM, the large but slower RAM chip inside the ESP32-S3's
 * package. 64 KB would be a sizeable share of the ~512 KB of internal RAM,
 * and nothing reads the ring by DMA or from an interrupt, which would need
 * internal RAM; the tasks that read it are not slowed noticeably by PSRAM.
 *
 * Positions. The writer keeps `s_head`, the number of samples written since
 * capture started; each reader keeps its own `pos`, the number of the next
 * sample it wants. Sample number n is stored at index n % AUDIO_RING_SAMPLES.
 * Both counters are 32-bit and wrap round to 0 after about 74 hours. The
 * arithmetic stays correct across the wrap because AUDIO_RING_SAMPLES is a
 * power of two (it divides 2^32 exactly, so the index of a sample does not
 * jump at the wrap) and because only differences between counters are used,
 * which unsigned arithmetic gets right however the counters wrapped.
 *
 * Locking. A mutex ("mutual exclusion" lock: one task holds it at a time,
 * others wait) covers the ring and s_head. The capture task holds it while
 * adding a chunk, a reader while copying its samples out, so a reader never
 * sees a chunk half-written. Copies are short, so a waiting capture task is
 * delayed by about a millisecond at most, well within the 75 ms it can wait
 * before the microphone driver drops audio (audio.h). FreeRTOS mutexes also
 * use "priority inheritance": while the high-priority capture task waits, a
 * lower-priority reader holding the lock runs at the capture task's
 * priority, so nothing in between can keep it waiting longer.
 *
 * Context. The capture task runs on AUDIO_CORE (core 1), away from the WiFi
 * interrupts on core 0, at a priority above every other audio task, because
 * it must read the microphone at least every 75 ms or audio is lost.
 * Readers run in their own tasks, on either core.
 */
#include "audio_ring.h"

#include <stdbool.h>
#include <string.h>
#include "audio.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "audio_ring";

/* Samples the capture task reads from the microphone at a time: 15 ms at
 * 16 kHz, one of the I2S driver's DMA buffers (audio.c) */
#define CHUNK               240

/* The capture task's longest wait for the microphone. Far longer than one
 * chunk takes to record, so it expires only if the hardware has stalled. */
#define IO_TIMEOUT_MS       1000

/* After a failed read the task pauses this long before trying again, so a
 * microphone that fails at once every time cannot keep core 1 busy */
#define RETRY_DELAY_MS      100

/* Priority 12 (ARCHITECTURE.md, task table): above the wake-word task and
 * the audio test tasks, which read from the ring, so the ring is always
 * filled before anyone waits on it. Its stack needs little: the chunk buffer
 * is static, not a local variable. */
#define TASK_PRIORITY       12
#define TASK_STACK          3072    /* bytes */

/* Event group bit set each time a chunk is added. An event group is a set of
 * flags that tasks can wait on; setting a flag wakes every task waiting for
 * it, which is what lets several readers wait for the same new audio. */
#define BIT_NEW_AUDIO       (1 << 0)

static SemaphoreHandle_t s_lock;
static EventGroupHandle_t s_events;

/* The ring itself, in PSRAM, and the count of samples written to it since
 * capture started. Both only under s_lock. */
static int16_t *s_ring;
static uint32_t s_head;

/* One chunk from the microphone, before it is copied into the ring. In
 * internal RAM, and used only by the capture task. */
static int16_t s_chunk[CHUNK];

/* Copies `n` samples into the ring starting at sample number `pos`, in two
 * pieces when they run past the end of the block. Caller holds s_lock. */
static void ring_write(uint32_t pos, const int16_t *src, size_t n)
{
    size_t at = pos % AUDIO_RING_SAMPLES;
    size_t first = n < AUDIO_RING_SAMPLES - at ? n : AUDIO_RING_SAMPLES - at;
    memcpy(&s_ring[at], src, first * sizeof(int16_t));
    memcpy(s_ring, src + first, (n - first) * sizeof(int16_t));
}

/* The reverse of ring_write(). Caller holds s_lock. */
static void ring_copy_out(uint32_t pos, int16_t *dst, size_t n)
{
    size_t at = pos % AUDIO_RING_SAMPLES;
    size_t first = n < AUDIO_RING_SAMPLES - at ? n : AUDIO_RING_SAMPLES - at;
    memcpy(dst, &s_ring[at], first * sizeof(int16_t));
    memcpy(dst + first, s_ring, (n - first) * sizeof(int16_t));
}

static void capture_task(void *arg)
{
    bool failing = false;   /* logs each run of failures once, not every 100 ms */

    for (;;) {
        /* Read into s_chunk first, without the lock: the read blocks for
         * about 15 ms, and readers must not wait that long */
        esp_err_t err = audio_capture_read(s_chunk, CHUNK, IO_TIMEOUT_MS);
        if (err != ESP_OK) {
            if (!failing) {
                ESP_LOGE(TAG, "microphone read failed: %s", esp_err_to_name(err));
                failing = true;
            }
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
            continue;
        }
        if (failing) {
            ESP_LOGI(TAG, "microphone reading again");
            failing = false;
        }

        xSemaphoreTake(s_lock, portMAX_DELAY);
        ring_write(s_head, s_chunk, CHUNK);
        s_head += CHUNK;
        xSemaphoreGive(s_lock);

        /* Wake every reader waiting for audio, then lower the flag again so
         * the next wait blocks until the next chunk. A reader that finds too
         * little audio just before this pair and starts waiting just after
         * it misses the wake-up and waits for the next chunk, 15 ms later;
         * it never waits longer than that. */
        xEventGroupSetBits(s_events, BIT_NEW_AUDIO);
        xEventGroupClearBits(s_events, BIT_NEW_AUDIO);
    }
}

esp_err_t audio_ring_start(void)
{
    ESP_RETURN_ON_FALSE(s_ring == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    s_lock = xSemaphoreCreateMutex();
    s_events = xEventGroupCreate();
    /* heap_caps_calloc() is calloc() with a choice of memory: MALLOC_CAP_SPIRAM
     * asks for PSRAM. Zeroed, so a reader started back in time before enough
     * has been captured reads silence rather than leftover memory. */
    s_ring = heap_caps_calloc(AUDIO_RING_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (s_lock == NULL || s_events == NULL || s_ring == NULL
        || xTaskCreatePinnedToCore(capture_task, "capture", TASK_STACK, NULL,
                                   TASK_PRIORITY, NULL, AUDIO_CORE) != pdPASS) {
        /* Release whatever was created, so a failure leaves nothing behind */
        if (s_lock) vSemaphoreDelete(s_lock);
        if (s_events) vEventGroupDelete(s_events);
        free(s_ring);
        s_lock = NULL;
        s_events = NULL;
        s_ring = NULL;
        ESP_LOGE(TAG, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

/* Sets a reader `back` samples before the newest audio, limited as
 * described in audio_ring.h. Caller holds s_lock. */
static void place_reader(audio_ring_reader_t *r, uint32_t back)
{
    if (back > AUDIO_RING_READ_MAX) {
        back = AUDIO_RING_READ_MAX;
    }
    /* Nothing before the first sample: s_head is also the number of samples
     * captured so far, until it first wraps after about 74 hours */
    if (back > s_head) {
        back = s_head;
    }
    r->pos = s_head - back;
}

void audio_ring_reader_init(audio_ring_reader_t *r, uint32_t back_ms)
{
    /* Milliseconds to samples. 64-bit, since a large back_ms would overflow
     * 32 bits when multiplied by the rate; then limited to fit 32 bits. */
    uint64_t back = (uint64_t)back_ms * AUDIO_CAPTURE_RATE / 1000;
    if (back > AUDIO_RING_READ_MAX) {
        back = AUDIO_RING_READ_MAX;
    }
    /* Before audio_ring_start() there is no lock and no audio: start at 0 */
    if (s_lock == NULL) {
        r->pos = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    place_reader(r, (uint32_t)back);
    xSemaphoreGive(s_lock);
}

void audio_ring_reader_init_at(audio_ring_reader_t *r, uint32_t pos)
{
    if (s_lock == NULL) {
        r->pos = 0;
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* How far back `pos` is. Unsigned subtraction, correct across the
     * counters' wrap (see the top of this file). */
    place_reader(r, s_head - pos);
    xSemaphoreGive(s_lock);
}

esp_err_t audio_ring_read(audio_ring_reader_t *r, int16_t *out, size_t samples,
                          uint32_t timeout_ms, uint32_t *dropped)
{
    ESP_RETURN_ON_FALSE(s_ring != NULL, ESP_ERR_INVALID_STATE, TAG, "not started");
    ESP_RETURN_ON_FALSE(samples <= AUDIO_RING_READ_MAX, ESP_ERR_INVALID_ARG, TAG,
                        "read of %u samples is too long", (unsigned)samples);

    TickType_t start = xTaskGetTickCount();
    TickType_t limit = pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        /* How far the reader is behind the writer. Unsigned subtraction,
         * correct across the counters' wrap. */
        uint32_t behind = s_head - r->pos;
        if (behind > AUDIO_RING_SAMPLES) {
            /* The audio the reader wanted is already overwritten: skip to
             * the oldest still held */
            uint32_t skip = behind - AUDIO_RING_SAMPLES;
            r->pos += skip;
            behind = AUDIO_RING_SAMPLES;
            if (dropped) {
                *dropped += skip;
            }
        }
        if (behind >= samples) {
            ring_copy_out(r->pos, out, samples);
            r->pos += samples;
            xSemaphoreGive(s_lock);
            return ESP_OK;
        }
        xSemaphoreGive(s_lock);

        /* Not enough audio yet: wait for the next chunk, but not past the
         * caller's time limit */
        TickType_t waited = xTaskGetTickCount() - start;
        if (waited >= limit) {
            return ESP_ERR_TIMEOUT;
        }
        /* Arguments: the flag to wait for, do not clear it on return (the
         * capture task does that, since other readers wait for it too), wait
         * for all listed flags (there is only one), and the longest wait */
        xEventGroupWaitBits(s_events, BIT_NEW_AUDIO, pdFALSE, pdTRUE, limit - waited);
    }
}
