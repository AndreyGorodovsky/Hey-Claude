#include "console.h"

#include <stdio.h>
#include <string.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "linenoise/linenoise.h"

static const char *TAG = "console";

static int cmd_reboot(int argc, char **argv)
{
    esp_restart();
    return 0;
}

static void print_keys(void)
{
    for (size_t i = 0; i < app_config_key_count(); i++) {
        printf("%s%s", i ? ", " : "Keys: ", app_config_key_name(i));
    }
    printf("\n");
}

static int config_show(void)
{
    /* What is stored, which may differ from what is running until reboot */
    for (size_t i = 0; i < app_config_key_count(); i++) {
        const char *key = app_config_key_name(i);
        bool secret = false;
        app_config_key_info(key, &secret);
        char value[APP_CFG_VALUE_MAX + 1];
        esp_err_t err = app_config_read_stored(key, value, sizeof(value));
        if (err == ESP_ERR_NOT_FOUND) {
            printf("  %-13s (not set)\n", key);
        } else if (err != ESP_OK) {
            printf("  %-13s (unreadable: %s)\n", key, esp_err_to_name(err));
        } else {
            printf("  %-13s %s\n", key, secret ? "(set, hidden)" : value);
        }
    }
    printf("  running device_id: %s\n", app_config_get()->device_id);
    return 0;
}

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
        /* The REPL records each line before running it; drop the history,
         * this line included, so the value cannot be recalled */
        linenoiseHistoryFree();
    }

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

esp_err_t console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_cfg = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_cfg.prompt = "hey-claude>";
    /* Commands are read from UART0, the board's CH340 port. The native USB
     * port also shows logs but does not accept input. */
    esp_console_dev_uart_config_t uart_cfg = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
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
        .help = "Show or change stored settings. Values containing spaces go in "
                "double quotes; write a backslash as \\\\ and a quote as \\\". "
                "Keys: wifi_ssid, wifi_pass, server_url, device_id, device_token",
        .hint = "show | set <key> <value> | unset <key>",
        .func = cmd_config,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&config), TAG, "config");

    return esp_console_start_repl(repl);
}
