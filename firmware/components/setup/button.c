/*
 * The BOOT button. See button.h for what it is for and why reading GPIO 0
 * is safe once the firmware runs.
 *
 * The pin is looked at every 50 ms by a periodic software timer (esp_timer),
 * rather than through an interrupt. A button bounces: its contacts make and
 * break several times in the first few milliseconds of a press, and an
 * interrupt would see each bounce as a press. Looking every 50 ms sees a
 * pressed button as pressed, whatever it did in between, and costs almost
 * nothing.
 *
 * Context: button_watch() runs once at boot; the timer callback runs in the
 * esp_timer task.
 */
#include "button.h"

#include <stdbool.h>
#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_timer.h"

static const char *TAG = "button";

/* How often the pin is looked at, and how long it must be held, in
 * milliseconds. Three seconds is long enough that a knock or a brush past
 * the board does not count, and short enough to wait out on purpose. */
#define POLL_MS             50
#define HOLD_MS             3000

static void (*s_on_hold)(void);
static esp_timer_handle_t s_timer;
/* Used only by the timer callback */
static uint32_t s_held_ms;
static bool s_fired;

static void poll(void *arg)
{
    /* Pressed pulls the pin to ground: 0 is pressed */
    bool pressed = gpio_get_level(BOARD_BOOT_BUTTON) == 0;
    if (!pressed) {
        s_held_ms = 0;
        s_fired = false;
        return;
    }
    s_held_ms += POLL_MS;
    if (s_held_ms >= HOLD_MS && !s_fired) {
        /* Once per press: a button still held after it counted does not
         * count again */
        s_fired = true;
        s_on_hold();
    }
}

esp_err_t button_watch(void (*on_hold)(void))
{
    ESP_RETURN_ON_FALSE(s_timer == NULL, ESP_ERR_INVALID_STATE, TAG, "already watching");
    s_on_hold = on_hold;

    /* An input with the chip's own pull-up resistor, which holds the pin
     * high while the button is up. The board has one of its own as well;
     * two in parallel do no harm. */
    const gpio_config_t io = {
        .pin_bit_mask = 1ULL << BOARD_BOOT_BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pin");
    /* A press already under way when watching starts is the one that
     * restarted the chip into setup mode, still held. It must be let go
     * before it can count, or the press that opened setup would also close
     * it. Set before the timer exists, so no callback can see it unset. */
    s_fired = gpio_get_level(BOARD_BOOT_BUTTON) == 0;

    const esp_timer_create_args_t args = { .callback = poll, .name = "boot_button" };
    ESP_RETURN_ON_ERROR(esp_timer_create(&args, &s_timer), TAG, "timer");
    /* esp_timer counts in microseconds */
    return esp_timer_start_periodic(s_timer, POLL_MS * 1000);
}
