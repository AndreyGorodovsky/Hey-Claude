/*
 * The controls on the breadboard: a push button and a slide switch (pins in
 * board.h).
 *
 * The button interrupts a turn: pressed while the device listens, thinks or
 * speaks, it ends the turn and returns the device to IDLE. The switch is
 * reserved for continuous conversation, which is planned and not yet built;
 * for now its position is only reported.
 *
 * A press is announced as CONTROLS_EVENT / CONTROLS_BUTTON_PRESSED on the
 * default event loop, the moment the button goes down, once its contacts
 * have settled. Holding it announces nothing more, and a new press counts
 * only after a release. Like the wake-word component, this one only reports:
 * what a press means in each state belongs to the state machine.
 *
 * Threading: controls_start() is called once at boot. controls_get() may be
 * called from any task.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(CONTROLS_EVENT);

enum {
    CONTROLS_BUTTON_PRESSED,    /* no event data */
};

/* How the controls stand, for the console */
typedef struct {
    bool button_down;           /* the button is held now */
    bool switch_closed;         /* the switch connects its pin to ground: the
                                   position toward the connected outer leg */
    uint32_t presses;           /* presses since boot, announced or not */
    uint32_t lost;              /* presses not announced: the event queue was full */
} controls_state_t;

/* Sets up both pins and starts watching them. Call once at boot, after the
 * default event loop exists. A button already held at that moment counts
 * only once it has been let go and pressed again. */
esp_err_t controls_start(void);

/* How the controls stand. Before controls_start() has succeeded, all false
 * and zero. */
void controls_get(controls_state_t *out);

#ifdef __cplusplus
}
#endif
