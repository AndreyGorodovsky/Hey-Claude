/*
 * Brownout guard.
 *
 * A brownout is the supply voltage dipping below what the chip needs. The
 * chip's brownout detector then restarts it (sdkconfig.defaults, and
 * KNOWN-ISSUES R2). If the supply is too weak for the device to get going,
 * every restart ends in the next brownout, for ever, and the screen never
 * settles on anything a person could read.
 *
 * The guard counts brownouts that follow one another closely. At the third
 * it tells the boot sequence (main.c) to stop before the parts that draw the
 * most: WiFi is not joined and nothing is played. The device then rests
 * with the reason on its screen and its console working, until it is powered
 * off and on or restarted by hand.
 *
 * What it cannot do. The boot sequence asks the guard after the display,
 * the microphone and wake-word detection have started, so that the reason
 * can be shown. A supply so weak that the chip browns out before that point
 * still restarts without end; the guard covers a supply that fails when the
 * radio or the amplifier comes on, which is where the current peaks.
 *
 * The count is kept in the chip's "RTC memory", a small area that keeps its
 * contents through a restart but not through a loss of power. Nothing is
 * written to flash, and nothing kept is a setting or a credential.
 *
 * Not yet seen with a real brownout: the guard was tested on the bench by a
 * build that counted ordinary restarts as brownouts (STATUS.md, stage 7).
 * That the count survives a real one rests on ESP-IDF's handling of it: the
 * detector's interrupt restarts the chip by software, which leaves RTC
 * memory as it was.
 */
#include "brownout_guard.h"

#include <stdint.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"

static const char *TAG = "brownout";

/* Brownouts in a row at which the guard trips */
#define BROWNOUTS_MAX       3
/* A device that has run this long, in seconds, is taken to have started
 * well, and its earlier brownouts are forgotten. A later brownout, in the
 * middle of a loud reply for example, then counts as the first again. */
#define FORGET_AFTER_S      60
/* Marks the counter below as written by this firmware: "BROW" in ASCII */
#define MAGIC               0x42524F57u

/* RTC_NOINIT_ATTR places a variable in RTC memory and tells the start-up
 * code not to clear it. After power-on it holds whatever happened to be
 * there, which is what the magic number is for. */
static RTC_NOINIT_ATTR uint32_t s_magic;
static RTC_NOINIT_ATTR uint32_t s_brownouts;

/* Runs in the esp_timer task, FORGET_AFTER_S after boot. One store to a
 * variable nothing else reads after boot, so it needs no lock. */
static void forget(void *arg)
{
    s_brownouts = 0;
}

bool brownout_guard_tripped(void)
{
    esp_reset_reason_t reason = esp_reset_reason();
    /* The second is another detector's name for a disturbed supply; this
     * chip does not report it, and it is listed for the ones that do */
    bool brownout = reason == ESP_RST_BROWNOUT || reason == ESP_RST_PWR_GLITCH;
    if (!brownout) {
        /* Any other restart, the console's `reboot` among them, starts the
         * count again: that is how a tripped guard is cleared by hand */
        s_brownouts = 0;
    } else if (s_magic != MAGIC) {
        s_brownouts = 1;    /* the counter held nothing of ours yet */
    } else {
        s_brownouts++;
    }
    s_magic = MAGIC;

    if (s_brownouts >= BROWNOUTS_MAX) {
        /* The count is left as it is, so that a further brownout trips the
         * guard again at once */
        ESP_LOGE(TAG, "%lu brownouts in a row. The supply or cable is too weak "
                      "(R2); change it, then power off and on",
                 (unsigned long)s_brownouts);
        return true;
    }
    if (s_brownouts > 0) {
        /* Started after a brownout, but not yet too many. esp_timer is
         * ESP-IDF's software timer service; a one-shot timer calls its
         * function once, from the service's own task. If the timer cannot
         * be made or started the count is simply kept until the next
         * restart of another kind, which errs on the side of the guard. */
        const esp_timer_create_args_t args = {
            .callback = forget,
            .name = "brownouts",
        };
        esp_timer_handle_t timer;
        if (esp_timer_create(&args, &timer) != ESP_OK
            /* The time is given in microseconds */
            || esp_timer_start_once(timer, (uint64_t)FORGET_AFTER_S * 1000000) != ESP_OK) {
            ESP_LOGW(TAG, "brownout count will not be forgotten");
        }
    }
    return false;
}
