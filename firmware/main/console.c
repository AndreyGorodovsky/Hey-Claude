/*
 * Serial console: a command line on the board's UART port.
 *
 * A UART is the chip's basic serial port. On this board, UART0 is wired to a
 * CH340 USB-to-serial chip behind the USB-C port marked "UART", which appears
 * on the computer as a COM port. `idf.py monitor` opens that port, shows the
 * log, and sends typed characters back to the chip.
 *
 * ESP-IDF's esp_console component provides the REPL (read-eval-print loop):
 * it runs in its own FreeRTOS task, reads a line, splits it into words like a
 * shell (argc/argv), and calls the function registered for the first word.
 * Line editing and up-arrow history come from the bundled linenoise library.
 *
 * Commands:
 *   help                          list commands (provided by esp_console)
 *   config show                   print the stored settings; secrets hidden
 *   config set <key> <value>      validate and store a setting
 *   config unset <key>            remove a stored setting
 *   reboot                        restart, applying changed settings
 *   audio ...                     audio tests, listed in audio_cmd.c
 *   state, display ...            the device state and the display test, in ui_cmd.c
 *   wake ...                      wake-word counters and tuning, in wake_cmd.c
 *   link                          the server connection, in link_cmd.c
 *   temp                          the chip's temperature, in temp_cmd.c
 *   mem                           free memory and stack headroom, in mem_cmd.c
 */
#include "console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_config.h"
#include "audio_cmd.h"
#include "link_cmd.h"
#include "mem_cmd.h"
#include "temp_cmd.h"
#include "ui_cmd.h"
#include "wake_cmd.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "linenoise/linenoise.h"

static const char *TAG = "console";

/* Command handlers run in the console task. They return 0 on success and
 * non-zero on failure, like a shell command's exit status. */

static int cmd_reboot(int argc, char **argv)
{
    /* Runs registered shutdown handlers (WiFi stops cleanly), then resets the
     * chip. The next boot logs the reset reason as "software restart". */
    esp_restart();
    return 0;   /* not reached */
}

/* Prints "Keys: a, b, c" for error messages. */
static void print_keys(void)
{
    for (size_t i = 0; i < app_config_key_count(); i++) {
        printf("%s%s", i ? ", " : "Keys: ", app_config_key_name(i));
    }
    printf("\n");
}

static int config_show(void)
{
    /* This reads what is stored in NVS, not what is running. The two differ
     * after a `config set` until the next reboot, which is why the running
     * device_id is printed separately at the end. */
    for (size_t i = 0; i < app_config_key_count(); i++) {
        const char *key = app_config_key_name(i);
        bool secret = false;
        app_config_key_info(key, &secret);
        char value[APP_CFG_VALUE_MAX + 1];   /* +1 for the terminating NUL */
        esp_err_t err = app_config_read_stored(key, value, sizeof(value));
        if (err == ESP_ERR_NOT_FOUND) {
            printf("  %-13s (not set)\n", key);
        } else if (err != ESP_OK) {
            printf("  %-13s (unreadable: %s)\n", key, esp_err_to_name(err));
        } else {
            /* Secrets are never printed, only whether they are set */
            printf("  %-13s %s\n", key, secret ? "(set, hidden)" : value);
        }
    }
    printf("  running device_id: %s\n", app_config_get()->device_id);
    return 0;
}

/*
 * Handles `config ...`. The console has already split the line into words;
 * quoted values arrive as one word with the quotes removed. For example,
 * `config set wifi_ssid "My Net"` gives argc = 4 and
 * argv = {"config", "set", "wifi_ssid", "My Net"}.
 */
static int cmd_config(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "show") == 0) {
        return config_show();
    }
    bool set = argc == 4 && strcmp(argv[1], "set") == 0;
    bool unset = argc == 3 && strcmp(argv[1], "unset") == 0;
    if (!set && !unset) {
        printf("usage: config show | config set <key> <value> | config unset <key>\n");
        return 1;
    }

    const char *key = argv[2];
    bool secret;
    if (!app_config_key_info(key, &secret)) {
        printf("unknown key '%s'. ", key);
        print_keys();
        return 1;
    }
    if (secret) {
        /* The REPL adds each line to its up-arrow history before running the
         * command, so this very line, password included, is already there.
         * Clearing the whole history is the only way to remove it. */
        linenoiseHistoryFree();
    }

    /* Validation lives in app_config, not here, so any future way of
     * entering settings applies the same rules */
    const char *reason = NULL;
    esp_err_t err = set ? app_config_set(key, argv[3], &reason) : app_config_unset(key);
    if (err == ESP_ERR_INVALID_ARG && reason) {
        printf("%s not saved: %s\n", key, reason);
        return 1;
    }
    if (err != ESP_OK) {
        printf("failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("%s %s. Reboot to apply.\n", key, set ? "saved" : "cleared");
    return 0;
}

bool console_parse_int(const char *s, int min, int max, int *out)
{
    /* strtol() stops at the first character that is not a digit and points
     * `end` at it, so anything left over, as in "5x", means the text was not
     * a number */
    char *end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != '\0' || v < min || v > max) {
        return false;
    }
    *out = (int)v;
    return true;
}

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hey-claude>";
    /* Commands are read from UART0, the board's CH340 port, at the default
     * 115200 baud. The native USB port also shows the log (ESP-IDF mirrors
     * output there) but does not accept input. */
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    /* ESP_RETURN_ON_ERROR: if the call fails, log it under TAG with the given
     * message and return the error from this function. */
    ESP_RETURN_ON_ERROR(esp_console_new_repl_uart(&uart_cfg, &repl_cfg, &repl), TAG, "repl");

    ESP_RETURN_ON_ERROR(esp_console_register_help_command(), TAG, "help");

    const esp_console_cmd_t reboot = {
        .command = "reboot",
        .help = "Restart the device, applying any changed settings",
        .func = cmd_reboot,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&reboot), TAG, "reboot");

    const esp_console_cmd_t config = {
        .command = "config",
        /* The escapes are for C: on screen this reads
         * "write a backslash as \\ and a quote as \"" */
        .help = "Show or change stored settings. Values containing spaces go in "
                "double quotes; write a backslash as \\\\ and a quote as \\\". "
                "Keys: wifi_ssid, wifi_pass, server_url, device_id, device_token",
        .hint = "show | set <key> <value> | unset <key>",
        .func = cmd_config,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&config), TAG, "config");
    ESP_RETURN_ON_ERROR(audio_cmd_register(), TAG, "audio");
    ESP_RETURN_ON_ERROR(ui_cmd_register(), TAG, "state and display");
    ESP_RETURN_ON_ERROR(wake_cmd_register(), TAG, "wake");
    ESP_RETURN_ON_ERROR(link_cmd_register(), TAG, "link");
    ESP_RETURN_ON_ERROR(temp_cmd_register(), TAG, "temp");
    ESP_RETURN_ON_ERROR(mem_cmd_register(), TAG, "mem");

    /* Starts the console task and returns; the task runs from now on */
    return esp_console_start_repl(repl);
}
