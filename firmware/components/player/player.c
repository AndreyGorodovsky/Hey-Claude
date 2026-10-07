/*
 * Reply playback. See player.h for what it offers; this file is how.
 *
 * Two tasks meet here. The WebSocket client's task, on core 0, puts audio
 * into the ring buffer (player_feed). The playback task, created here on
 * core 1 with the rest of the audio chain, takes it out and writes it to
 * the amplifier. The amplifier's driver takes samples only as fast as they
 * play, so the playback task runs in real time by itself; it needs no
 * timer.
 *
 * The buffer and the few variables describing the reply are shared by both
 * tasks and guarded by one mutex. It is held only for the length of a
 * memory copy, well under a millisecond, so neither task holds the other
 * up.
 *
 * A "generation" number ties everything to one reply. It goes up at every
 * player_begin(), player_chime() and player_abort(). The playback task
 * notes the number when it starts on a reply and stops as soon as it
 * changes, so a reply that was aborted, or replaced by a new one, can never
 * be confused with the one now wanted.
 *
 * The chime (player.h) is played as if it were a very short reply: one
 * "playback" is either a reply or the chime, and the functions below named
 * for a playback serve both. Only the reports at the end differ.
 */
#include "player.h"

#include <stdbool.h>
#include <string.h>
#include "audio.h"
#include "chime.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "player";

/* The highest playback rate the amplifier driver accepts (audio.h). The
 * buffer is sized for it, so any rate the server may announce fits: 384 KB
 * for 4 s, in PSRAM, which has megabytes to spare. A reply at 24 kHz uses
 * half of it. */
#define RATE_MAX            48000
#define BYTES_PER_SAMPLE    2

/* Samples written to the amplifier at a time. 480 samples is 20 ms at
 * 24 kHz: small enough that an abort is acted on quickly. */
#define CHUNK_SAMPLES       480
/* Audio that must be waiting before a reply starts to play, in
 * milliseconds. A little, so that the first few network chunks arriving
 * unevenly do not cause a gap in the first word. */
#define PREROLL_MS          80
/* How often the playback task looks again when it has nothing to play */
#define IDLE_POLL_MS        20
/* Longest wait for the amplifier to take one chunk; it normally takes a
 * chunk within the chunk's own length */
#define WRITE_TIMEOUT_MS    500

/* Playback level, in percent of the level the server sends. Real volume
 * control arrives in stage 8; until then this is the one value. 40 and
 * then 60 were found too quiet on the breadboard (2026-10-06), so the
 * audio is now played exactly as sent. That is the loudest this constant
 * can make it; more needs the amplifier's own gain raised (its GAIN pin)
 * or the audio boosted before playing, with care not to clip. The supply
 * has been tested with a full-scale tone, the worst case (KNOWN-ISSUES R2). */
#define VOLUME_PERCENT      100

/* The chime's size in the ring. What it sounds like is chime.c's business. */
#define CHIME_BYTES         (CHIME_SAMPLES * BYTES_PER_SAMPLE)

#define TASK_STACK          4096
/* Recorded in ARCHITECTURE.md's task table: above wake-word detection, so
 * a model run cannot delay the speaker, and below microphone capture */
#define TASK_PRIO           10

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;

/* Set once at start and read-only afterwards */
static player_report_fn s_report;
static uint32_t s_buffer_ms;
static uint32_t s_stall_ms;
static size_t s_buf_bytes;      /* size of the ring */
static int16_t *s_chime_pcm;    /* the chime: CHIME_BYTES bytes in PSRAM */

/* ---- Shared between tasks, guarded by s_lock ---- */

static uint8_t *s_buf;          /* the ring: s_buf_bytes bytes in PSRAM */
static size_t s_head;           /* where the next byte is written */
static size_t s_fill;           /* bytes waiting to be played */
static size_t s_room;           /* bytes this reply may hold: 4 s at its rate */
static uint32_t s_rate;         /* samples per second of this reply */
static uint32_t s_gen;          /* the reply everything above belongs to */
static bool s_active;           /* a reply, or the chime, is under way */
static bool s_is_chime;         /* it is the chime, not a reply */
static bool s_ended;            /* player_end() has been called for it */
static bool s_overflowed;       /* a feed did not fit */
static size_t s_peak_fill;      /* the most that has waited at once, in bytes */

/* Wakes the playback task if it is waiting for something to do */
static void nudge(void)
{
    xTaskNotifyGive(s_task);
}

/* False until player_init() has succeeded. The public functions do nothing
 * before then, so a device whose audio failed to start still runs. */
static bool ready(void)
{
    return s_task != NULL;
}

/* Starts afresh for a reply or the chime, discarding whatever was
 * under way. Call with s_lock held. */
static void begin_locked(uint32_t sample_rate, bool is_chime)
{
    s_gen++;
    s_rate = sample_rate;
    /* Clamped, so that a rate the amplifier will refuse anyway cannot make
     * the buffer accept more than it is */
    uint32_t rate = sample_rate > RATE_MAX ? RATE_MAX : sample_rate;
    s_room = (size_t)rate * BYTES_PER_SAMPLE * s_buffer_ms / 1000;
    s_head = 0;
    s_fill = 0;
    s_peak_fill = 0;
    s_active = true;
    s_is_chime = is_chime;
    s_ended = false;
    s_overflowed = false;
}

void player_begin(uint32_t sample_rate)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    begin_locked(sample_rate, false);
    xSemaphoreGive(s_lock);
    nudge();
}

void player_chime(void)
{
    if (!ready()) {
        return;
    }
    /* A whole reply in one step: begun, filled and ended under the lock, so
     * the playback task sees all of it or none of it. Being already ended,
     * it plays out and stops by itself. */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    begin_locked(CHIME_RATE, true);
    memcpy(s_buf, s_chime_pcm, CHIME_BYTES);
    s_head = CHIME_BYTES;
    s_fill = CHIME_BYTES;
    s_peak_fill = CHIME_BYTES;
    s_ended = true;
    xSemaphoreGive(s_lock);
    nudge();
}

void player_feed(const uint8_t *pcm, size_t len)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active && !s_overflowed) {
        if (len > s_room - s_fill) {
            s_overflowed = true;
        } else {
            /* The ring wraps: what does not fit before the end of the
             * buffer continues at its start */
            size_t first = s_buf_bytes - s_head;
            if (first > len) {
                first = len;
            }
            memcpy(s_buf + s_head, pcm, first);
            memcpy(s_buf, pcm + first, len - first);
            s_head = (s_head + len) % s_buf_bytes;
            s_fill += len;
            if (s_fill > s_peak_fill) {
                s_peak_fill = s_fill;
            }
        }
    }
    xSemaphoreGive(s_lock);
    nudge();
}

void player_end(void)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        s_ended = true;
    }
    xSemaphoreGive(s_lock);
    nudge();
}

void player_abort(void)
{
    if (!ready()) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) {
        s_gen++;
        s_active = false;
        s_fill = 0;
    }
    xSemaphoreGive(s_lock);
    nudge();
}

/* What the playback task found when it looked at the buffer */
typedef enum {
    TAKE_AUDIO,     /* samples were copied out */
    TAKE_EMPTY,     /* nothing to play yet; more may come */
    TAKE_FINISHED,  /* ended, and everything has been played */
    TAKE_OVERFLOW,  /* a feed did not fit */
    TAKE_OVER,      /* this reply was aborted or replaced */
} take_t;

/* Copies up to CHUNK_SAMPLES whole samples out of the ring into `out`.
 * With `need_ms` above zero, holds back until that much audio is waiting,
 * unless the reply has ended. */
static take_t take(uint32_t gen, int16_t *out, size_t *samples, uint32_t need_ms)
{
    take_t result;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    size_t need = (size_t)s_rate * BYTES_PER_SAMPLE * need_ms / 1000;
    /* Whole samples only: a feed can end half-way through one */
    size_t n = s_fill - s_fill % BYTES_PER_SAMPLE;
    if (n > CHUNK_SAMPLES * BYTES_PER_SAMPLE) {
        n = CHUNK_SAMPLES * BYTES_PER_SAMPLE;
    }
    if (gen != s_gen) {
        result = TAKE_OVER;
    } else if (s_overflowed) {
        result = TAKE_OVERFLOW;
    } else if (n == 0 || (s_fill < need && !s_ended)) {
        result = (s_ended && n == 0) ? TAKE_FINISHED : TAKE_EMPTY;
    } else {
        /* The oldest byte is s_fill behind the write position */
        size_t tail = (s_head + s_buf_bytes - s_fill) % s_buf_bytes;
        size_t first = s_buf_bytes - tail;
        if (first > n) {
            first = n;
        }
        memcpy(out, s_buf + tail, first);
        memcpy((uint8_t *)out + first, s_buf, n - first);
        s_fill -= n;
        *samples = n / BYTES_PER_SAMPLE;
        result = TAKE_AUDIO;
    }
    xSemaphoreGive(s_lock);
    return result;
}

/* Marks the playback as over, unless a newer one has already begun. Returns
 * the most audio that waited at once during it, in milliseconds. */
static uint32_t close_playback(uint32_t gen, uint32_t rate)
{
    uint32_t peak_ms = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (gen == s_gen) {
        s_active = false;
        s_fill = 0;
        if (rate > 0) {
            peak_ms = (uint32_t)((uint64_t)s_peak_fill * 1000 / BYTES_PER_SAMPLE / rate);
        }
    }
    xSemaphoreGive(s_lock);
    return peak_ms;
}

/* Plays one playback to its end, its failure or its abort. Runs in the
 * playback task. With `is_chime` it is the chime that is played:
 * the same steps, with one report at the end in place of a reply's. */
static void play_out(uint32_t gen, uint32_t rate, bool is_chime)
{
    /* static: 960 bytes that need not be on the task's stack. Only this
     * task uses it. */
    static int16_t chunk[CHUNK_SAMPLES];
    bool amp_on = false;
    uint32_t empty_ms = 0;      /* how long the buffer has been empty just now */
    player_failure_t failure = PLAYER_FAIL_DEVICE;
    bool failed = false;
    bool finished = false;
    /* For the line logged at the end: how the reply went, as evidence for
     * the buffer's size and the server's pacing */
    uint32_t played_ms = 0;
    uint32_t silent_ms = 0;     /* time the speaker was on with nothing to play */
    uint32_t underruns = 0;     /* times it ran dry after sound had begun */

    for (;;) {
        size_t samples = 0;
        /* The preroll applies only until the amplifier is on */
        take_t got = take(gen, chunk, &samples, amp_on ? 0 : PREROLL_MS);
        if (got == TAKE_OVER) {
            break;
        }
        if (got == TAKE_FINISHED) {
            finished = true;
            break;
        }
        if (got == TAKE_OVERFLOW) {
            failure = PLAYER_FAIL_OVERFLOW;
            failed = true;
            break;
        }
        if (got == TAKE_EMPTY) {
            /* Nothing to play. While the amplifier is on, its driver sends
             * silence by itself. Waits for a feed, or looks again shortly. */
            if (amp_on && empty_ms == 0) {
                underruns++;
            }
            if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IDLE_POLL_MS)) == 0) {
                empty_ms += IDLE_POLL_MS;
                silent_ms += amp_on ? IDLE_POLL_MS : 0;
            }
            if (empty_ms >= s_stall_ms) {
                failure = PLAYER_FAIL_STALLED;
                failed = true;
                break;
            }
            continue;
        }
        empty_ms = 0;

        if (!amp_on) {
            esp_err_t err = audio_play_start(rate);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "amplifier not started at %lu Hz: %s",
                         (unsigned long)rate, esp_err_to_name(err));
                failure = PLAYER_FAIL_DEVICE;
                failed = true;
                break;
            }
            amp_on = true;
            if (!is_chime) {
                s_report(PLAYER_STARTED, 0);
            }
        }
        /* Scale to the playback level. The product of two 16-bit values
         * needs 32 bits; dividing brings it back into range. */
        for (size_t i = 0; i < samples; i++) {
            chunk[i] = (int16_t)((int32_t)chunk[i] * VOLUME_PERCENT / 100);
        }
        esp_err_t err = audio_play_write(chunk, samples, WRITE_TIMEOUT_MS);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "amplifier write failed: %s", esp_err_to_name(err));
            failure = PLAYER_FAIL_DEVICE;
            failed = true;
            break;
        }
        played_ms += (uint32_t)(samples * 1000 / rate);
    }

    if (amp_on) {
        /* Lets the last queued audio out, about 70 ms at 24 kHz, then
         * switches the amplifier off */
        audio_play_stop();
    }
    uint32_t peak_ms = close_playback(gen, rate);
    const char *how = failed ? "failed" : finished ? "finished" : "stopped";
    if (is_chime) {
        /* The buffer's figures say nothing about a sound put in whole */
        ESP_LOGI(TAG, "chime %s: played %lu ms", how, (unsigned long)played_ms);
    } else {
        ESP_LOGI(TAG, "reply %s: played %lu ms; most held at once %lu ms of %lu; "
                      "ran dry %lu times, silent %lu ms",
                 how, (unsigned long)played_ms, (unsigned long)peak_ms,
                 (unsigned long)s_buffer_ms, (unsigned long)underruns,
                 (unsigned long)silent_ms);
    }
    /* A report is made if the playback ran to its end or failed, as found
     * in the loop above. An abort that arrives after that, while the
     * amplifier was being stopped, does not take the report back: the
     * receiver must judge a report by the state it is in itself. */
    if (is_chime) {
        /* Reported whether or not it could be played: the request waits on
         * this report, and matters more than the sound. */
        if (failed || finished) {
            s_report(PLAYER_CHIME_DONE, finished ? 1 : 0);
        }
    } else if (failed) {
        s_report(PLAYER_FAILED, failure);
    } else if (finished) {
        s_report(PLAYER_FINISHED, 0);
    }
}

static void player_task(void *arg)
{
    for (;;) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        bool active = s_active;
        uint32_t gen = s_gen;
        uint32_t rate = s_rate;
        bool is_chime = s_is_chime;
        xSemaphoreGive(s_lock);
        if (active) {
            play_out(gen, rate, is_chime);
        } else {
            /* Sleeps until a reply begins. Looked at before sleeping, not
             * after: a reply that began while the last one was being wound
             * up has already used up its wake-up call. One that begins
             * between the look and the sleep leaves its call pending, and
             * the sleep returns at once. */
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        }
    }
}

esp_err_t player_init(const player_config_t *cfg)
{
    ESP_RETURN_ON_FALSE(s_task == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");
    ESP_RETURN_ON_FALSE(cfg && cfg->on_report && cfg->buffer_ms > 0 && cfg->stall_ms > 0,
                        ESP_ERR_INVALID_ARG, TAG, "incomplete configuration");
    /* The chime must fit in the room a playback at its rate is given.
     * Checked before anything is allocated. */
    ESP_RETURN_ON_FALSE((size_t)CHIME_RATE * BYTES_PER_SAMPLE * cfg->buffer_ms / 1000
                        >= CHIME_BYTES,
                        ESP_ERR_INVALID_ARG, TAG, "buffer too small for the chime");
    s_report = cfg->on_report;
    s_buffer_ms = cfg->buffer_ms;
    s_stall_ms = cfg->stall_ms;
    s_buf_bytes = (size_t)RATE_MAX * BYTES_PER_SAMPLE * s_buffer_ms / 1000;

    /* From here on a failure frees what was taken before it, so that the
     * device, which carries on without a speaker, does not carry the
     * buffers with it. `ret` and the `fail` label are what the
     * ESP_GOTO_ON_FALSE macro uses: it sets one and jumps to the other. */
    esp_err_t ret = ESP_OK;
    /* MALLOC_CAP_SPIRAM asks for PSRAM, the large external RAM, keeping the
     * small internal RAM for what needs it */
    s_buf = heap_caps_malloc(s_buf_bytes, MALLOC_CAP_SPIRAM);
    s_chime_pcm = heap_caps_malloc(CHIME_BYTES, MALLOC_CAP_SPIRAM);
    ESP_GOTO_ON_FALSE(s_buf != NULL && s_chime_pcm != NULL, ESP_ERR_NO_MEM, fail, TAG,
                      "no PSRAM for the buffers");
    chime_make(s_chime_pcm);
    s_lock = xSemaphoreCreateMutex();
    ESP_GOTO_ON_FALSE(s_lock != NULL, ESP_ERR_NO_MEM, fail, TAG, "no memory");
    BaseType_t made = xTaskCreatePinnedToCore(player_task, "player", TASK_STACK, NULL,
                                              TASK_PRIO, &s_task, AUDIO_CORE);
    ESP_GOTO_ON_FALSE(made == pdPASS, ESP_ERR_NO_MEM, fail, TAG, "no memory for the task");
    return ESP_OK;

fail:
    /* free() of NULL does nothing, so it does not matter how far it got */
    heap_caps_free(s_buf);
    heap_caps_free(s_chime_pcm);
    s_buf = NULL;
    s_chime_pcm = NULL;
    if (s_lock != NULL) {
        vSemaphoreDelete(s_lock);
        s_lock = NULL;
    }
    s_task = NULL;
    return ret;
}
