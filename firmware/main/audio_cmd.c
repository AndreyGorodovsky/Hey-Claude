/*
 * Console commands for testing audio on the hardware.
 *
 *   audio loop [seconds]             record (default 3 s), print the level,
 *                                    then play the recording back
 *   audio tone <percent> [seconds]   play a 440 Hz tone at that share of full
 *                                    scale (default 3 s), for measuring the
 *                                    supply's peak current (KNOWN-ISSUES R2)
 *
 * A task is a FreeRTOS thread: an independent piece of the program that the
 * scheduler runs on one of the chip's two CPU cores. cmd_audio() runs in the
 * console's own task and only parses the command. The audio itself runs in a
 * short-lived task created for the test and "pinned" to core 1, meaning the
 * scheduler never moves it to the other core. Core 1 is reserved for audio in
 * ARCHITECTURE.md, away from the WiFi work on core 0. The command returns at
 * once and the test task prints its results as it goes, then deletes itself.
 * One test runs at a time.
 */
#include "audio_cmd.h"

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_cmd";

#define SECONDS_DEFAULT     3
/* Recordings are kept in PSRAM, the 8 MB RAM chip inside the ESP32-S3's
 * package: plentiful but slower than the ~512 KB of internal RAM. 10 s of
 * 16 kHz 16-bit audio is 320 KB, more than internal RAM could spare. */
#define SECONDS_MAX         10

#define TONE_HZ             440.0f  /* the note A, as in the bring-up amp_test */
#define TONE_RATE           24000   /* spoken replies play at this rate */

/* The loopback plays the recording louder so it can be heard: gain is chosen
 * to bring the loudest sample up to half of full scale, but never more than
 * 64 times (+36 dB), so a silent recording is not turned into loud hiss. */
#define LOOP_TARGET_PEAK    16384.0f    /* half of 32768, the 16-bit full scale */
#define LOOP_MAX_GAIN       64.0f

/* Longest wait for the microphone or amplifier before a test gives up. Far
 * longer than one DMA buffer (15 ms), so it fires only if the hardware stalls. */
#define IO_TIMEOUT_MS       1000

/* Samples handed to the audio functions per call: 15 ms at 16 kHz */
#define CHUNK               240

/* Every task has its own stack, a fixed block of RAM for its local variables
 * and function calls, sized when the task is created. printf of floating
 * point needs about 2 KB, and the 480-byte chunk buffer lives there too. */
#define TASK_STACK          4096    /* bytes */
/* When several tasks are ready on a core, the scheduler runs the one with the
 * highest priority number. Above the console (2), so typing cannot stall
 * audio; below the WiFi driver (23, shown in the boot log), which must keep
 * up with the radio. */
#define TASK_PRIORITY       10
/* Test tasks run on AUDIO_CORE, from audio.h, alongside the I2S interrupts */

/* True while a test task exists. Set by the console task only when false, and
 * cleared by the test task only when true, so the two never write at the same
 * moment. `volatile` because they run on different cores. */
static volatile bool s_busy;

/* Parameters for the running test. Written by the console task only after
 * it has checked that no test is running, and before it creates the test
 * task, which only reads them; so they need no lock. */
static struct {
    int seconds;
    int percent;    /* tone only */
} s_job;

/* Converts a sample level to dBFS: decibels relative to full scale, where
 * 0 dBFS is the loudest a 16-bit sample can be and every -6 dB halves it. */
static float dbfs(float level)
{
    return level > 0 ? 20.0f * log10f(level / 32768.0f) : -INFINITY;
}

/* Clamps to the 16-bit range, so an over-amplified sample clips instead of
 * wrapping round to the opposite sign, which would sound like a crack. */
static int16_t clip16(float v)
{
    if (v > 32767.0f) return 32767;
    if (v < -32768.0f) return -32768;
    return (int16_t)v;
}

/* Ends a test task. vTaskDelete(NULL) deletes the calling task and does not
 * return; callers still write `return` after it so the flow reads plainly. */
static void finish(void)
{
    s_busy = false;
    vTaskDelete(NULL);
}

static void loop_task(void *arg)
{
    size_t frames = (size_t)s_job.seconds * AUDIO_CAPTURE_RATE;
    /* heap_caps_malloc() is malloc() with a choice of memory; MALLOC_CAP_SPIRAM
     * asks for PSRAM. The recording is too large for internal RAM, and nothing
     * reads it with DMA or from an interrupt, so PSRAM's slower access is fine.
     * It is released with the ordinary free(). */
    int16_t *rec = heap_caps_malloc(frames * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (rec == NULL) {
        printf("audio loop: not enough PSRAM for %d s\n", s_job.seconds);
        finish();
        return;
    }

    printf("Recording %d s. Speak now.\n", s_job.seconds);
    /* Drop what was buffered before the command, so the recording starts now */
    esp_err_t err = audio_capture_flush();
    if (err == ESP_OK) {
        err = audio_capture_read(rec, frames, IO_TIMEOUT_MS);
    }
    if (err != ESP_OK) {
        printf("audio loop: recording failed: %s\n", esp_err_to_name(err));
        free(rec);
        finish();
        return;
    }

    /* Level of the raw recording, before any gain: RMS (the average power,
     * what the ear hears as loudness) and peak (the single loudest sample) */
    float peak = 0;
    double sum = 0, sum_sq = 0;
    for (size_t i = 0; i < frames; i++) {
        float v = fabsf((float)rec[i]);
        if (v > peak) peak = v;
        sum += rec[i];
        sum_sq += (double)rec[i] * rec[i];
    }
    float rms = sqrtf((float)(sum_sq / frames));
    /* The mean should be near zero. A large one is a constant offset (DC)
     * that inflates the RMS figure without being audible. */
    float mean = (float)(sum / frames);
    float gain = peak > 0 ? LOOP_TARGET_PEAK / peak : 1.0f;
    if (gain > LOOP_MAX_GAIN) gain = LOOP_MAX_GAIN;
    if (gain < 1.0f) gain = 1.0f;
    printf("Level: RMS %.1f dBFS, peak %.1f dBFS, mean %.0f. "
           "Playing back %.1fx louder (+%.1f dB).\n",
           dbfs(rms), dbfs(peak), mean, gain, 20.0f * log10f(gain));

    /* Playback at the capture rate, so the recording needs no conversion */
    err = audio_play_start(AUDIO_CAPTURE_RATE);
    if (err == ESP_OK) {
        int16_t chunk[CHUNK];
        for (size_t pos = 0; pos < frames && err == ESP_OK; pos += CHUNK) {
            size_t n = frames - pos < CHUNK ? frames - pos : CHUNK;
            for (size_t i = 0; i < n; i++) {
                chunk[i] = clip16(rec[pos + i] * gain);
            }
            err = audio_play_write(chunk, n, IO_TIMEOUT_MS);
        }
        /* Stopped even after a failed write, so the amplifier is switched off */
        esp_err_t stop = audio_play_stop();
        if (err == ESP_OK) err = stop;
    }
    if (err == ESP_OK) {
        printf("Done.\n");
    } else {
        printf("audio loop: playback failed: %s\n", esp_err_to_name(err));
    }

    free(rec);
    finish();
}

static void tone_task(void *arg)
{
    size_t frames = (size_t)s_job.seconds * TONE_RATE;
    float amp = 32767.0f * s_job.percent / 100.0f;
    /* Phase advance per sample, in radians: one full cycle (2 pi) every
     * TONE_RATE / TONE_HZ samples */
    const float step = 2.0f * (float)M_PI * TONE_HZ / TONE_RATE;
    float phase = 0;

    printf("Tone: %.0f Hz at %d%% for %d s\n", TONE_HZ, s_job.percent, s_job.seconds);
    esp_err_t err = audio_play_start(TONE_RATE);
    if (err == ESP_OK) {
        int16_t chunk[CHUNK];
        /* frames is a whole number of seconds at 24 kHz, a multiple of CHUNK */
        for (size_t pos = 0; pos < frames && err == ESP_OK; pos += CHUNK) {
            for (size_t i = 0; i < CHUNK; i++) {
                chunk[i] = clip16(amp * sinf(phase));
                phase += step;
                if (phase > 2.0f * (float)M_PI) {
                    phase -= 2.0f * (float)M_PI;    /* wrap to keep float precision */
                }
            }
            err = audio_play_write(chunk, CHUNK, IO_TIMEOUT_MS);
        }
        /* Stopped even after a failed write, so the amplifier is switched off */
        esp_err_t stop = audio_play_stop();
        if (err == ESP_OK) err = stop;
    }
    if (err == ESP_OK) {
        printf("Done.\n");
    } else {
        printf("audio tone: failed: %s\n", esp_err_to_name(err));
    }
    finish();
}

/* Parses a whole number in [min, max]; false if the text is anything else.
 * strtol() stops at the first character that is not a digit and points `end`
 * at it, so anything left over, as in "5x", means the text was not a number. */
static bool parse_int(const char *s, int min, int max, int *out)
{
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (int)v;
    return true;
}

/* Handles `audio ...`. Runs in the console task, which has already split the
 * line into words (see console.c). Returns 0 on success, like a shell command. */
static int cmd_audio(int argc, char **argv)
{
    bool loop = argc >= 2 && argc <= 3 && strcmp(argv[1], "loop") == 0;
    bool tone = argc >= 3 && argc <= 4 && strcmp(argv[1], "tone") == 0;
    int secs_arg = loop ? 2 : 3;    /* position of the optional seconds */
    /* Parsed into locals: s_job may belong to a test that is still running */
    int seconds = SECONDS_DEFAULT;
    int percent = 0;
    if ((!loop && !tone)
        || (tone && !parse_int(argv[2], 1, 100, &percent))
        || (argc > secs_arg && !parse_int(argv[secs_arg], 1, SECONDS_MAX, &seconds))) {
        printf("usage: audio loop [seconds] | audio tone <percent 1-100> [seconds]; "
               "seconds 1-%d, default %d\n", SECONDS_MAX, SECONDS_DEFAULT);
        return 1;
    }
    if (s_busy) {
        printf("an audio test is already running\n");
        return 1;
    }

    s_busy = true;
    s_job.seconds = seconds;
    s_job.percent = percent;
    /* Arguments: the task's function, a name shown in diagnostics, stack
     * size, a parameter for the function (unused; it reads s_job), priority,
     * a place to store the task's handle (not needed), and the core. */
    if (xTaskCreatePinnedToCore(loop ? loop_task : tone_task, "audio_test", TASK_STACK,
                                NULL, TASK_PRIORITY, NULL, AUDIO_CORE) != pdPASS) {
        s_busy = false;
        printf("could not start the audio task: out of memory\n");
        return 1;
    }
    return 0;
}

esp_err_t audio_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "audio",
        .help = "Test audio. 'loop' records then plays back; 'tone' plays 440 Hz "
                "at a given share of full volume",
        .hint = "loop [seconds] | tone <percent> [seconds]",
        .func = cmd_audio,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "audio");
    return ESP_OK;
}
