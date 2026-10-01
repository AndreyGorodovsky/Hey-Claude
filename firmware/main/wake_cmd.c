/*
 * Console command for watching and tuning wake-word detection.
 *
 *   wake                   detections, scores and timing since the last
 *                          reset, with the rate per hour for false-accept runs
 *   wake reset             clear the counters, to start a measurement
 *   wake log on|off        log the highest score once a second
 *   wake cutoff <0-1>      change the cutoff until the next reboot
 *
 * A false-accept run (KNOWN-ISSUES R1) is `wake reset`, then hours of
 * background sound with nobody saying the phrase, then `wake`: every
 * detection counted is a false one.
 *
 * Context: the handler runs in the console task. The counters belong to the
 * wakeword component, which hands out consistent copies.
 */
#include "wake_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_check.h"
#include "esp_console.h"
#include "wakeword.h"

static const char *TAG = "wake_cmd";

static int wake_status(void)
{
    wakeword_stats_t st;
    wakeword_get_stats(&st);
    uint32_t secs = st.seconds;
    float hours = secs / 3600.0f;

    printf("wake word '%s'%s, cutoff %.3f\n", wakeword_phrase(),
           st.running ? "" : ", NOT RUNNING (see the boot log)", st.cutoff);
    printf("  over %lu h %02lu min: %lu detections",
           (unsigned long)(secs / 3600), (unsigned long)(secs / 60 % 60),
           (unsigned long)st.detections);
    if (hours > 0) {
        printf(" (%.2f per hour)", st.detections / hours);
    }
    printf(", peak score %.2f\n", st.peak_score);
    printf("  model: %lu runs, %.1f ms average, %.1f ms longest\n",
           (unsigned long)st.inferences, st.infer_us_avg / 1000.0f, st.infer_us_max / 1000.0f);
    printf("  memory: arena %u of %u bytes used, stack %lu bytes never used\n",
           (unsigned)st.arena_used, (unsigned)st.arena_size, (unsigned long)st.stack_unused);
    printf("  audio lost: %lu samples\n", (unsigned long)st.dropped);
    return 0;
}

/* Handles `wake ...`. Returns 0 on success, like a shell command. */
static int cmd_wake(int argc, char **argv)
{
    if (argc == 1) {
        return wake_status();
    }
    if (argc == 2 && strcmp(argv[1], "reset") == 0) {
        wakeword_reset_stats();
        printf("Counters cleared.\n");
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "log") == 0
        && (strcmp(argv[2], "on") == 0 || strcmp(argv[2], "off") == 0)) {
        wakeword_set_log(strcmp(argv[2], "on") == 0);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "cutoff") == 0) {
        /* strtof() stops at the first character that is not part of a
         * number; anything left over means the text was not one */
        char *end;
        float cutoff = strtof(argv[2], &end);
        if (end != argv[2] && *end == '\0' && wakeword_set_cutoff(cutoff) == ESP_OK) {
            /* Read back: the cutoff is kept in steps of 1/255, so the value
             * in use can differ slightly from the one typed */
            wakeword_stats_t st;
            wakeword_get_stats(&st);
            printf("Cutoff %.3f until reboot.\n", st.cutoff);
            return 0;
        }
    }
    printf("usage: wake | wake reset | wake log on|off | "
           "wake cutoff <0.002-0.998, e.g. 0.95>\n");
    return 1;
}

esp_err_t wake_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "wake",
        .help = "Show wake-word counters and timing, clear them, log scores, or change "
                "the cutoff until reboot",
        .hint = "[reset | log on|off | cutoff <0-1>]",
        .func = cmd_wake,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "wake");
    return ESP_OK;
}
