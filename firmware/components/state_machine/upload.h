/*
 * The request upload: tells the server a request has begun, then sends the
 * microphone's audio while the request is being captured. Private to the
 * state_machine component.
 */
#pragma once

#include <stdint.h>
#include "esp_err.h"

/* Why an upload stopped by itself */
typedef enum {
    UPLOAD_FAIL_SEND,           /* the request could not be sent to the server */
    UPLOAD_FAIL_MICROPHONE,     /* no audio is arriving from the microphone */
} upload_failure_t;

/* Creates the upload task, which then waits. `on_failed` is called from
 * that task if an upload cannot go on; it must not block. */
esp_err_t upload_init(void (*on_failed)(upload_failure_t why));

/* Starts a request: sends `utterance_start`, then audio from `pos`, a
 * position in the audio ring (audio_ring.h): the point where the wake word
 * ended. Audio captured between then and now goes at once, and the rest as
 * it is captured. Returns at once; the sending is done by the upload task. */
void upload_start(uint32_t pos);

/* Stops sending. The frame being sent at that moment still goes; the
 * protocol expects a few frames after the server's `stop_capture`. Safe to
 * call when nothing is being sent. */
void upload_stop(void);
