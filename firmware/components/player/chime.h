/*
 * The chime: the two short notes the device plays when it hears the wake
 * word, before it starts to listen to the request.
 *
 * It is computed, not stored as a recording, so the repository holds no
 * audio file. This header and chime.c are everything that decides how it
 * sounds; the player (player.c) only plays the samples. Private to the
 * player component.
 */
#pragma once

#include <stdint.h>

/* Samples per second the chime is made at. 24 kHz is the rate of spoken
 * replies; any rate the amplifier accepts would do. */
#define CHIME_RATE          24000
#define CHIME_NOTES         2
/* Length of each note, in milliseconds. Short on purpose: the request is
 * not listened to until the chime is over, so every millisecond here is
 * one the person waits before speaking. */
#define CHIME_NOTE_MS       70
#define CHIME_NOTE_SAMPLES  (CHIME_RATE * CHIME_NOTE_MS / 1000)
/* The whole chime, in 16-bit samples */
#define CHIME_SAMPLES       (CHIME_NOTES * CHIME_NOTE_SAMPLES)

/* Fills `out` with the chime: CHIME_SAMPLES samples, 16-bit signed mono.
 * Uses floating point, so call it once at start, from a task, and keep the
 * result. */
void chime_make(int16_t *out);
