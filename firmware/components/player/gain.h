/*
 * The playback level: how much louder or quieter than the server sent it a
 * playback is made, and a limiter that keeps the result from clipping.
 * Private to the player; player.c applies it to each chunk on its way to
 * the amplifier.
 *
 * A decibel (dB) is the usual measure of a change in loudness: +6 dB
 * doubles every sample, -6 dB halves it, and 3 dB is about the smallest
 * step a listener clearly hears as louder or quieter.
 *
 * Clipping is what happens when a sample made louder no longer fits in 16
 * bits: it is cut off at the largest value, and the cut is heard as a
 * crackle. The limiter prevents it. When a sample would go above a ceiling
 * just under full scale, the limiter turns the level down at once by
 * exactly enough, then lets it back up gradually over a tenth of a second.
 * Quiet passages are untouched; only the loudest peaks are pressed down.
 *
 * Context: used only by the playback task.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

/* The state of the level for one playback. Started afresh for each, so
 * that one reply's loud ending does not hold the next one's start down. */
typedef struct {
    float gain;         /* the multiplier asked for */
    float reduction;    /* the limiter's own multiplier now: 1 when idle */
    float release;      /* how far `reduction` returns toward 1 per sample */
    uint32_t limited;   /* samples the limiter pressed down */
    float deepest;      /* the smallest `reduction` reached */
} gain_t;

/* Starts a playback at `db` decibels, of audio at `rate` samples per second */
void gain_init(gain_t *g, int db, uint32_t rate);

/* Applies the level to `n` samples in place */
void gain_apply(gain_t *g, int16_t *pcm, size_t n);
