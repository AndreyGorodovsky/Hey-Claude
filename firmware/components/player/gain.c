/*
 * The playback level and its limiter. See gain.h for what they are.
 *
 * Floating point is used throughout. The ESP32-S3 has a hardware unit for
 * single-precision floats, and at 24,000 samples a second the few
 * operations per sample below are a negligible load. ESP-IDF saves a
 * task's float registers when it is switched out, so a float in a task is
 * as safe as an integer.
 */
#include "gain.h"

#include <math.h>

/* The highest the limiter lets a sample reach: 1 dB under full scale
 * (32767 x 0.89). Kept under full scale because the amplifier's own
 * filters can add a little overshoot to a sample that is at it. */
#define CEILING             29200.0f
/* How long the limiter takes to let the level back up after a peak, in
 * milliseconds: the time to recover about two thirds of the way. Short
 * enough that the words after a loud syllable are not left quieter,
 * long enough that the level does not flutter on every cycle of a low
 * voice, which is heard as distortion. */
#define RELEASE_MS          100.0f

void gain_init(gain_t *g, int db, uint32_t rate)
{
    /* Decibels to a multiplier: 10 to the power of db/20 */
    g->gain = powf(10.0f, (float)db / 20.0f);
    g->reduction = 1.0f;
    /* The fraction of the remaining distance to 1 recovered per sample, for
     * an exponential return with the time constant above */
    g->release = rate > 0 ? 1.0f - expf(-1000.0f / (RELEASE_MS * (float)rate)) : 1.0f;
    g->limited = 0;
    g->deepest = 1.0f;
}

void gain_apply(gain_t *g, int16_t *pcm, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        float x = (float)pcm[i] * g->gain;
        float size = fabsf(x);
        if (size * g->reduction > CEILING) {
            /* Turned down at once, by exactly enough: this sample lands on
             * the ceiling, and no sample ever goes above it */
            g->reduction = CEILING / size;
            g->limited++;
            if (g->reduction < g->deepest) {
                g->deepest = g->reduction;
            }
        }
        /* Within 16 bits by construction: at most the ceiling either way */
        pcm[i] = (int16_t)lrintf(x * g->reduction);
        g->reduction += (1.0f - g->reduction) * g->release;
    }
}
