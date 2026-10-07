/*
 * How the chime sounds. See chime.h for what it is for.
 *
 * Two notes, the second higher, which is the pattern people read as "go
 * ahead". Each is a sine wave, the plainest tone there is, faded in and out
 * so that it does not click.
 */
#include "chime.h"

#include <math.h>

/* The notes E5 and A5, in hertz: a rising fourth */
static const float NOTE_HZ[CHIME_NOTES] = { 660.0f, 880.0f };
/* Each note fades in and out over this long, in milliseconds. A note that
 * starts or stops at full strength is heard as a click. */
#define EDGE_MS             8
/* Loudness, in percent of full scale (the largest value a 16-bit sample
 * can hold). A reply's peaks reach full scale; a steady note at that level
 * would be far louder than speech. Accepted by ear on the breadboard on
 * 2026-10-07. The player's playback level is applied on top of this, which
 * changes nothing while that level is 100 %; whether the chime should
 * follow a volume control is to be decided with stage 8 (STATUS.md, open
 * decisions). */
#define LEVEL_PERCENT       30

void chime_make(int16_t *out)
{
    const int edge = CHIME_RATE * EDGE_MS / 1000;
    const float peak = 32767.0f * LEVEL_PERCENT / 100;

    for (int note = 0; note < CHIME_NOTES; note++) {
        /* How far the wave moves through its cycle per sample, in radians */
        const float step = 2.0f * (float)M_PI * NOTE_HZ[note] / CHIME_RATE;
        for (int i = 0; i < CHIME_NOTE_SAMPLES; i++) {
            /* 0 at the note's first and last sample, 1 in between */
            float gain = 1.0f;
            if (i < edge) {
                gain = (float)i / edge;
            } else if (i >= CHIME_NOTE_SAMPLES - edge) {
                gain = (float)(CHIME_NOTE_SAMPLES - 1 - i) / edge;
            }
            out[note * CHIME_NOTE_SAMPLES + i] = (int16_t)(peak * gain * sinf(step * i));
        }
    }
}
