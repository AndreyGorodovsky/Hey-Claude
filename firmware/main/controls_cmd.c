/*
 * Console command for the button and the switch on the breadboard.
 *
 *   controls        print how both stand, and the presses counted since boot
 *
 * For checking the wiring: run it with the button held and let go, and with
 * the switch in each position. Each press is also logged as it happens
 * ("controls: button pressed"), so a press that is counted twice, or not at
 * all, shows at once.
 *
 * Context: the handler runs in the console task.
 */
#include "controls_cmd.h"

#include <stdio.h>
#include "app_state.h"
#include "controls.h"
#include "esp_check.h"
#include "esp_console.h"

static const char *TAG = "controls_cmd";

static int cmd_controls(int argc, char **argv)
{
    controls_state_t s;
    controls_get(&s);
    printf("button: %s\n", s.button_down ? "pressed" : "up");
    printf("switch: %s\n", s.switch_closed ? "closed (toward the connected leg)"
                                           : "open (toward the free leg)");
    /* The mode in effect, as the state machine has recorded it: what the
     * switch means is its decision, not this command's */
    app_state_event_t st;
    app_state_get(&st);
    printf("continuous conversation: %s\n", st.continuous ? "on" : "off");
    printf("presses since boot: %lu", (unsigned long)s.presses);
    if (s.lost > 0) {
        printf(", %lu not announced", (unsigned long)s.lost);
    }
    printf("\n");
    return 0;
}

esp_err_t controls_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "controls",
        .help = "Show the button and the switch, and the presses counted",
        .func = cmd_controls,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "controls");
    return ESP_OK;
}
