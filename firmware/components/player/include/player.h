/*
 * Reply playback: holds the reply audio that has arrived but not yet
 * played, and plays it through the amplifier.
 *
 * Audio arrives from the network in bursts and plays at a fixed speed, so
 * something has to hold the difference. That is a ring buffer in PSRAM with
 * room for 4 s of audio. The server never sends more than 2 s ahead of
 * playback (docs/PROTOCOL.md, pacing); the other 2 s is margin for a stall
 * on the network, after which the delayed audio arrives together.
 *
 * One reply at a time goes through three calls: player_begin() when the
 * server announces it, player_feed() for each piece of audio, and
 * player_end() when the server says no more is coming. The player then
 * plays out what it holds and announces that it has finished.
 * player_abort() stops at once instead.
 *
 * What happens to a reply is reported through a function given at start.
 * A function and not an event on the default event loop, because there is
 * one receiver, and a call cannot be lost on the way.
 *
 * Threading: player_init() is called once at boot. Every other function may
 * be called from any task; player_begin() and player_feed() are called from
 * the WebSocket client's task and return quickly. Reports are made from the
 * playback task; the function receiving them must return quickly and must
 * not call back into the player.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

/* What the player reports, with the meaning of the value that comes with it */
typedef enum {
    PLAYER_STARTED,     /* the first sound of a reply is going to the speaker */
    PLAYER_FINISHED,    /* an ended reply has played to its last sample */
    PLAYER_FAILED,      /* the reply could not be played to its end; value: a
                           player_failure_t. Playback has stopped. */
    PLAYER_CHIME_DONE,    /* the chime is over and the speaker is
                           silent again; value: 1 if it was played, 0 if the
                           amplifier refused it */
} player_report_t;

typedef void (*player_report_fn)(player_report_t what, uint32_t value);

typedef enum {
    PLAYER_FAIL_STALLED,    /* no audio arrived for too long before player_end() */
    PLAYER_FAIL_OVERFLOW,   /* audio arrived that there was no room for */
    PLAYER_FAIL_DEVICE,     /* the amplifier could not be started or written to */
} player_failure_t;

typedef struct {
    /* Reply audio to have room for, in milliseconds */
    uint32_t buffer_ms;
    /* The longest the buffer may stay empty in the middle of a reply before
     * the reply is given up as stalled, in milliseconds */
    uint32_t stall_ms;
    player_report_fn on_report;
} player_config_t;

/* Allocates the buffer in PSRAM and starts the playback task on the audio
 * core. Call once at boot, after audio_init() has succeeded. The two times
 * are the caller's to choose because they are the protocol's, which the
 * player knows nothing of. */
esp_err_t player_init(const player_config_t *cfg);

/* A reply begins, at `sample_rate` samples per second. Anything left of an
 * earlier reply is discarded. A rate the amplifier cannot play is reported
 * later, as PLAYER_FAILED with PLAYER_FAIL_DEVICE, not here. */
void player_begin(uint32_t sample_rate);

/* Adds `len` bytes of reply audio: 16-bit signed little-endian mono
 * samples. A piece may end in the middle of a sample. If there is no room,
 * nothing is added, playback stops and PLAYER_FAILED follows: the protocol
 * has the device close the connection in that case, not play a reply with
 * a hole in it. */
void player_feed(const uint8_t *pcm, size_t len);

/* No more audio is coming for this reply. What is held plays out, then
 * PLAYER_FINISHED is posted. */
void player_end(void);

/* Stops playback, of a reply or of the chime, within about 100 ms and
 * discards what is held. No report follows, except one already on its way:
 * a playback that had just ended or failed by itself may still report.
 * Whoever receives reports must judge each by the state it is in itself.
 * Does nothing when nothing is playing. */
void player_abort(void);

/* Plays the chime: two short rising notes (chime.h), computed once at
 * start, that tell the person the wake word was heard. It goes through the
 * same path as a reply, so it cannot collide with one: a reply that begins
 * meanwhile replaces it, and player_abort() stops it. PLAYER_CHIME_DONE
 * follows once the sound has left the speaker and the amplifier is off,
 * about 0.2 s after the call, unless it was replaced or stopped. None of a
 * reply's reports are made for it. */
void player_chime(void);
