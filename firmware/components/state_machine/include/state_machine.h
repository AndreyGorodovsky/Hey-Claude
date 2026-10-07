/*
 * The state machine: the one place that decides what the device does next.
 *
 * Everything else in the firmware either reports what happened or does what
 * it is told. Wake-word detection reports a detection; the link reports the
 * server's messages and the state of the connection; the player reports
 * that a reply started or finished. This component hears all of it and
 * decides: start a request, stop the upload, play the reply, show an error,
 * give up on a turn.
 *
 * It is the only writer of the device state (app_state.h) once it has
 * started, so the screen always shows one consistent story.
 *
 * The states and what moves between them are in ARCHITECTURE.md (state
 * machine) and docs/PROTOCOL.md; state_machine.c lists them where they are handled.
 *
 * The link and the player know nothing of this component. Whoever starts
 * them (main.c) gives them the four functions at the end of this file, and
 * that is how their reports and the reply's audio get here.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "server_link.h"
#include "player.h"

/* What the state machine is told at start about the parts it depends on */
typedef struct {
    bool speaker_ok;    /* the amplifier and the player started */
    bool listening_ok;  /* the microphone and wake-word detection started */
} state_machine_config_t;

/* Starts the state machine in its own task. Call once at boot, after the
 * default event loop exists, with the state already set to CONNECTING, and
 * before server_link_start(), so that no report from the link is missed. From here
 * on nothing else may call app_state_set(). */
esp_err_t state_machine_start(const state_machine_config_t *cfg);

/* For server_link_start(): the link's reports. May be called from any task. */
void state_machine_link_report(server_link_report_t what, uint32_t value);

/* For player_init(): the player's reports. May be called from any task. */
void state_machine_player_report(player_report_t what, uint32_t value);

/* For server_link_start(), as its audio sink. Reply audio goes to the player
 * through these, and only while the state machine is in a turn that awaits
 * a reply: audio arriving at any other time, such as the tail of a turn
 * the device has already given up, is discarded here and never reaches the
 * speaker. Called from the WebSocket client's task. */
void state_machine_reply_start(uint32_t sample_rate);
void state_machine_reply_data(const uint8_t *pcm, size_t len);
