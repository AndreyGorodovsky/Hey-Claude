/*
 * The playback volume: a level from PROTO_VOLUME_MIN to PROTO_VOLUME_MAX
 * (protocol.h), what it sounds like, and keeping it through a restart.
 * Private to the state_machine component.
 *
 * The level is the device's own. It states it with each request, and the
 * server may answer "louder" with a new one; the console can set one too.
 * Each level is a number of decibels handed to the player (player.h).
 *
 * The level is kept in NVS (non-volatile storage, ESP-IDF's key-value store
 * in flash) in a namespace of its own, apart from the settings that
 * app_config keeps: it changes while the device runs, and erasing the
 * settings should not reset it, nor it them.
 *
 * Context: volume_init(), volume_set() and volume_store() are called only
 * from the state machine's task, which makes it the level's one writer.
 * volume_level(), volume_on_scale() and volume_db() may be called from any
 * task.
 */
#pragma once

#include <stdbool.h>

/* Loads the stored level, or the default if none is stored, and gives it to
 * the player. Call once, before anything can play. */
void volume_init(void);

/* The level now; 0 before volume_init() */
unsigned volume_level(void);

/* Whether `level` is on the protocol's scale */
bool volume_on_scale(unsigned level);

/* The loudness of a level on the scale, in decibels for the player */
int volume_db(unsigned level);

/* Makes `level`, which must be on the scale, the level from the next reply
 * or chime on. Not stored yet: see volume_store(). */
void volume_set(unsigned level);

/* Stores the level if it has changed since it was last stored. Writing to
 * flash pauses the chip's use of flash and PSRAM for milliseconds, and
 * occasionally for tens of them when NVS has to erase a page; the audio
 * tasks stop meanwhile. So the state machine calls this only in IDLE,
 * where a lost moment of microphone audio at most delays a wake word, and
 * never during a reply. */
void volume_store(void);
