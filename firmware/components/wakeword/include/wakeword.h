/*
 * Wake-word detection: listens to the microphone all the time and announces
 * when the wake phrase is heard.
 *
 * Runs a microWakeWord model, a small neural network trained to recognise
 * one phrase, on the audio from audio_ring. Everything happens on the
 * device; no audio leaves it to detect the wake word.
 *
 * A detection is announced as WAKEWORD_EVENT / WAKEWORD_DETECTED on the
 * default event loop, with a wakeword_event_t as its data. This component
 * only reports what it heard: it does not change the device state. Deciding
 * what a detection means (ignore it while already busy, start a request
 * while idle) belongs to the state machine (app_state.h).
 *
 * Scores. For every 30 ms of audio the model gives a probability, from 0 to
 * 1, that the phrase has just been said. The detector averages the last few
 * of these and reports a detection when the average exceeds the "cutoff". A
 * higher cutoff means fewer false detections but more missed phrases; it is
 * tuned by measurement (KNOWN-ISSUES R1).
 *
 * Threading: wakeword_start() is called once at boot. The other functions
 * may be called from any task.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(WAKEWORD_EVENT);

enum {
    WAKEWORD_DETECTED,      /* event data: wakeword_event_t */
};

typedef struct {
    float score;            /* the averaged probability that crossed the cutoff, 0-1 */
    uint32_t end_pos;       /* where the phrase ended: the audio_ring position
                               just after the audio that set off the detection,
                               for audio_ring_reader_init_at() */
} wakeword_event_t;

/* The state of detection, for tuning and diagnostics. The counters run
 * from boot, or from the last wakeword_reset_stats(); the other fields are
 * how things stand now. */
typedef struct {
    /* Counters */
    uint32_t seconds;           /* time they have been counting */
    uint32_t detections;        /* wake phrases reported */
    float peak_score;           /* highest averaged probability, 0-1 */
    uint32_t dropped;           /* samples of audio lost because the task fell behind */
    uint32_t inferences;        /* times the model has run */
    uint32_t infer_us_avg;      /* average time one run of the model took, in microseconds */
    uint32_t infer_us_max;      /* longest run, in microseconds */
    /* Now */
    bool running;               /* false if detection never started (see the boot
                                   log) or stopped after the model failed */
    float cutoff;               /* current cutoff, 0-1 */
    size_t arena_used;          /* bytes of the model's working memory in use */
    size_t arena_size;          /* bytes of working memory allocated */
    uint32_t stack_unused;      /* bytes of the task's stack never used so far */
} wakeword_stats_t;

/* Loads the model and starts detection in its own task on AUDIO_CORE. Call
 * once at boot, after audio_ring_start() and after the default event loop
 * exists. Detections start about a second later, once the model has heard
 * enough audio. */
esp_err_t wakeword_start(void);

/* The phrase the built-in model listens for, as written in its manifest,
 * such as "Hey Jarvis" */
const char *wakeword_phrase(void);

/* Changes the cutoff while running, for tuning; `cutoff` is between 0 and 1,
 * exclusive. It is kept in steps of 1/255, and values that round to 0 or 1
 * are refused (ESP_ERR_INVALID_ARG). Not stored: every boot starts with the
 * model's own cutoff. */
esp_err_t wakeword_set_cutoff(float cutoff);

/* When on, the detection task logs the highest averaged probability once a
 * second, for watching the scores while speaking. */
void wakeword_set_log(bool on);

void wakeword_get_stats(wakeword_stats_t *out);

/* Clears the counters, including the peak score, and starts their time
 * again, to begin a measurement */
void wakeword_reset_stats(void);

#ifdef __cplusplus
}
#endif
