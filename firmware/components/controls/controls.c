/*
 * The button and the switch on the breadboard. See controls.h for what they
 * are for.
 *
 * Both pins are looked at every 10 ms by a periodic software timer
 * (esp_timer), rather than through an interrupt. A mechanical contact
 * "bounces": for the first few milliseconds after it moves, it makes and
 * breaks many times, and an interrupt would see each bounce as a press. Here
 * a pin's new level is believed only after it has read the same three times
 * in a row, 30 ms, which outlasts the bounce of a small button or switch and
 * is still too short to feel late. The BOOT button (setup component,
 * button.c) is watched the same way, more slowly, because it waits for a 3 s
 * hold and not for the moment of a press.
 *
 * Context: controls_start() runs once at boot, in the main task. poll() runs
 * in the esp_timer task, which also runs every other software timer and must
 * never be held up: nothing in it waits.
 */
#include "controls.h"

#include "board.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "controls";

ESP_EVENT_DEFINE_BASE(CONTROLS_EVENT);

/* How often the pins are looked at, in milliseconds, and how many readings
 * in a row a new level must hold before it is believed */
#define POLL_MS         10
#define STABLE_READS    3

/* One contact, as poll() sees it */
typedef struct {
    gpio_num_t pin;
    bool settled;       /* the level believed: true is closed */
    bool last;          /* the latest reading */
    uint8_t same;       /* readings in a row equal to `last`, up to STABLE_READS */
} contact_t;

static contact_t s_button = { .pin = BOARD_BUTTON };
static contact_t s_switch = { .pin = BOARD_SWITCH };
static esp_timer_handle_t s_timer;
/* A move of the switch not yet announced. A position is a level, not an
 * event: announcing it late is still right, so a move the event loop had no
 * room for is announced again on every poll until it goes. esp_timer task
 * only. */
static bool s_switch_pending;

/* What controls_get() reports. Written in the esp_timer task and read from
 * any, so guarded by a spinlock, the FreeRTOS lock for data shared between
 * tasks that may run on both cores at once. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static controls_state_t s_state;

/* Closed connects the pin to ground: 0 is closed */
static bool read_closed(gpio_num_t pin)
{
    return gpio_get_level(pin) == 0;
}

/* Takes one reading, and returns true when it settles the contact at a new
 * level */
static bool contact_update(contact_t *c)
{
    bool now = read_closed(c->pin);
    if (now != c->last) {
        c->last = now;
        c->same = 1;
        return false;
    }
    if (c->same < STABLE_READS) {
        c->same++;
    }
    if (c->same == STABLE_READS && now != c->settled) {
        c->settled = now;
        return true;
    }
    return false;
}

static void poll(void *arg)
{
    bool pressed = contact_update(&s_button) && s_button.settled;
    bool moved = contact_update(&s_switch);
    if (moved) {
        s_switch_pending = true;
    }

    bool lost = false;
    if (pressed) {
        /* Timeout 0: the esp_timer task must not wait for room in the
         * event queue. A press lost this way is counted, and is seen by the
         * person as a press that did nothing, to be pressed again. */
        lost = esp_event_post(CONTROLS_EVENT, CONTROLS_BUTTON_PRESSED, NULL, 0, 0) != ESP_OK;
    }

    taskENTER_CRITICAL(&s_lock);
    s_state.button_down = s_button.settled;
    s_state.switch_closed = s_switch.settled;
    if (pressed) {
        s_state.presses++;
        if (lost) {
            s_state.lost++;
        }
    }
    taskEXIT_CRITICAL(&s_lock);

    /* Announced only once the new position is stored above, since whoever
     * hears of the move reads it with controls_get(), possibly at once from
     * the other core */
    bool move_waits = false;
    if (s_switch_pending) {
        s_switch_pending =
            esp_event_post(CONTROLS_EVENT, CONTROLS_SWITCH_CHANGED, NULL, 0, 0) != ESP_OK;
        /* Said once per move, not on every retry */
        move_waits = moved && s_switch_pending;
    }

    /* Logged after the lock is let go: printing takes time, and nothing
     * else may wait on the lock meanwhile */
    if (lost) {
        ESP_LOGW(TAG, "button press not announced: event queue full");
    } else if (pressed) {
        ESP_LOGI(TAG, "button pressed");
    }
    if (move_waits) {
        ESP_LOGW(TAG, "switch move not announced yet: event queue full; retrying");
    }
    if (moved) {
        ESP_LOGI(TAG, "switch %s", s_switch.settled ? "closed" : "open");
    }
}

/* Takes the contact's present level as settled, so that a button held at
 * start is not taken for a press */
static void contact_init(contact_t *c)
{
    c->settled = c->last = read_closed(c->pin);
    c->same = STABLE_READS;
}

esp_err_t controls_start(void)
{
    ESP_RETURN_ON_FALSE(s_timer == NULL, ESP_ERR_INVALID_STATE, TAG, "already started");

    /* Inputs with the chip's own pull-up resistor, about 45 kΩ, which holds
     * each pin high while its contact is open. Strong enough for the short
     * wires of a breadboard; nothing else drives these pins. */
    const gpio_config_t io = {
        .pin_bit_mask = (1ULL << BOARD_BUTTON) | (1ULL << BOARD_SWITCH),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&io), TAG, "pins");
    /* The pull-up needs a moment to raise a pin left floating until now;
     * well under a millisecond, through the pin's few picofarads */
    esp_rom_delay_us(100);

    /* Before the timer exists, so that no callback sees them unset */
    contact_init(&s_button);
    contact_init(&s_switch);
    taskENTER_CRITICAL(&s_lock);
    s_state.button_down = s_button.settled;
    s_state.switch_closed = s_switch.settled;
    taskEXIT_CRITICAL(&s_lock);

    const esp_timer_create_args_t args = { .callback = poll, .name = "controls" };
    esp_err_t err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) {
        /* esp_timer counts in microseconds */
        err = esp_timer_start_periodic(s_timer, POLL_MS * 1000);
        if (err != ESP_OK) {
            esp_timer_delete(s_timer);
        }
    }
    if (err != ESP_OK) {
        /* Without the timer nothing would ever update the positions read
         * above. Reported as open instead, as before controls_start(): a
         * switch frozen closed would hold continuous conversation on for
         * good, while open leaves the wake word to start every request. */
        s_timer = NULL;
        taskENTER_CRITICAL(&s_lock);
        s_state = (controls_state_t){ 0 };
        taskEXIT_CRITICAL(&s_lock);
        ESP_LOGE(TAG, "timer: %s", esp_err_to_name(err));
    }
    return err;
}

void controls_get(controls_state_t *out)
{
    taskENTER_CRITICAL(&s_lock);
    *out = s_state;
    taskEXIT_CRITICAL(&s_lock);
}
