/*
 * The request upload: tells the server a request has begun, then sends the
 * microphone's audio while the request is being captured, and tells it when
 * the person interrupts the turn. Private to the state_machine component.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* What the upload reports by itself */
typedef enum {
    UPLOAD_FAIL_SEND,           /* the request could not be sent to the server */
    UPLOAD_FAIL_MICROPHONE,     /* no audio is arriving from the microphone */
    UPLOAD_CANCEL_SENT,         /* upload_cancel(): `cancel` has gone to the server */
    UPLOAD_CANCEL_NOT_NEEDED,   /* upload_cancel(): the request had never been
                                   announced, so the server has no turn to end */
    UPLOAD_CANCEL_FAILED,       /* upload_cancel(): `cancel` could not be sent */
} upload_report_t;

/* Creates the upload task, which then waits. `on_report` is called from
 * that task; it must not block. */
esp_err_t upload_init(void (*on_report)(upload_report_t what));

/* Starts a request: sends `utterance_start`, stating the playback level
 * `volume`, then audio from `pos`, a position in the audio ring
 * (audio_ring.h): the point where the wake word ended. Audio captured
 * between then and now goes at once, and the rest as it is captured.
 * Returns at once; the sending is done by the upload task. */
void upload_start(uint32_t pos, uint8_t volume);

/* Stops sending. The frame being sent at that moment still goes; the
 * protocol expects a few frames after the server's `stop_capture`. Safe to
 * call when nothing is being sent. */
void upload_stop(void);

/* Stops sending, as upload_stop() does, and tells the server the person has
 * interrupted the latest request's turn, by sending `cancel` once anything
 * still being sent for it has gone. Returns at once; the outcome is
 * reported as one of the UPLOAD_CANCEL_ reports. */
void upload_cancel(void);
