/*
 * The microphone's audio, shared: a capture task that reads the microphone
 * without pause, and a ring buffer in PSRAM holding the last 2 s of what it
 * heard.
 *
 * The microphone can have only one reader (audio.h), but several parts of
 * the firmware listen: wake-word detection all the time, the request upload
 * from stage 6 after the wake word, and the `audio loop` console test. Each
 * of them is a "reader" here. Every reader keeps its own position in the
 * stream, so readers never take audio from each other, and a slow reader
 * cannot hold up the others: when one falls more than 2 s behind, it is
 * moved forward and told how much it missed.
 *
 * Audio is 16-bit signed mono at AUDIO_CAPTURE_RATE (audio.h). Positions
 * and lengths are counted in samples, one per 1/16000 s. A position is the
 * number of a sample since capture started; it identifies one moment in the
 * stream, so a moment can be passed from one reader to another (the wake
 * word's end, from detection to the upload).
 *
 * Threading: audio_ring_start() is called once at boot. After that, any
 * number of tasks may read at once, each with its own reader; one reader
 * must not be used by two tasks at once.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* C linkage when included from C++ (the wakeword component), so the
 * function names match those compiled from C */
#ifdef __cplusplus
extern "C" {
#endif

/* How much audio the ring holds: 32768 samples, 2.048 s at 16 kHz. A power
 * of two, which keeps positions exact when the sample counter wraps round
 * (about every 74 hours; see audio_ring.c). */
#define AUDIO_RING_SAMPLES      32768

/* Longest read, and the furthest back in time a reader can start: half the
 * ring, about 1 s. Never the whole ring: its oldest audio is overwritten by
 * the next chunk captured, so a reader starting that far back would lose
 * audio before its first read. */
#define AUDIO_RING_READ_MAX     (AUDIO_RING_SAMPLES / 2)

/* A reader's position in the stream: the number of the next sample it will
 * read. Owned by the reader; set it with one of the init functions and
 * otherwise leave it alone. */
typedef struct {
    uint32_t pos;
} audio_ring_reader_t;

/* Allocates the ring in PSRAM and starts the capture task on AUDIO_CORE.
 * Call once at boot, after audio_init() has succeeded. */
esp_err_t audio_ring_start(void);

/* Points a reader at the stream. With `back_ms` 0 its first read starts with
 * sound from now; otherwise it starts that many milliseconds earlier, so
 * audio from just before the call is included. Limited to
 * AUDIO_RING_READ_MAX, and to what has been captured since boot. */
void audio_ring_reader_init(audio_ring_reader_t *r, uint32_t back_ms);

/* Points a reader at a position taken from the stream earlier, such as the
 * one in a wake-word detection. A position further back than
 * AUDIO_RING_READ_MAX is moved forward to that limit. */
void audio_ring_reader_init_at(audio_ring_reader_t *r, uint32_t pos);

/* Fills `out` with exactly `samples` samples (at most AUDIO_RING_READ_MAX)
 * from the reader's position, waiting until they have been captured, and
 * moves the reader on. Reading in a loop gives an unbroken stream, unless
 * the reader falls more than 2 s behind: it then skips forward to the
 * oldest audio still held, and the number of samples skipped is added to
 * `*dropped` if that is not NULL. Returns ESP_ERR_TIMEOUT, with nothing
 * read, if the audio has not arrived within `timeout_ms`. */
esp_err_t audio_ring_read(audio_ring_reader_t *r, int16_t *out, size_t samples,
                          uint32_t timeout_ms, uint32_t *dropped);

#ifdef __cplusplus
}
#endif
