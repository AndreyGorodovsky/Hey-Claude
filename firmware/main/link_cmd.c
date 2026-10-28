/*
 * Console commands for the server connection.
 *
 *   link        whether WiFi and the server connection are up, where the
 *               server was found, and how often the connection was made
 *   pair <code> store a pairing code from `python -m server pair` and
 *               restart, so that the link trades it for a token
 *
 * For telling apart "the device cannot find the server" from "the server
 * does not answer" when the screen shows CONNECTING or ERROR.
 *
 * Context: the handlers run in the console task.
 */
#include "link_cmd.h"

#include <stdio.h>
#include "app_config.h"
#include "esp_check.h"
#include "esp_console.h"
#include "esp_system.h"
#include "server_link.h"

static const char *TAG = "link_cmd";

static int cmd_link(int argc, char **argv)
{
    server_link_status_t st;
    server_link_get_status(&st);
    printf("WiFi:        %s\n", st.wifi ? "up" : "down");
    printf("Server:      %s\n", st.up ? "connected" : "not connected");
    /* The address is the server machine's on the local network */
    printf("Last tried:  %s\n", st.server[0] ? st.server : "(no server found yet)");
    printf("Connections: %lu made in %lu attempts since boot\n",
           (unsigned long)st.connections, (unsigned long)st.attempts);
    if (st.had_down) {
        printf("Last ended:  %s\n", server_link_down_name(st.last_down));
    }
    return 0;
}

static int cmd_pair(int argc, char **argv)
{
    if (argc != 2) {
        printf("Usage: pair <code>, with the digits python -m server pair printed\n");
        return 1;
    }
    /* The same check and storage as `config set pair_code`. The restart
     * follows at once: the code lasts minutes on the server. */
    const char *reason = NULL;
    esp_err_t err = app_config_set("pair_code", argv[1], &reason);
    if (err == ESP_ERR_INVALID_ARG) {
        printf("The pairing code %s.\n", reason);
        return 1;
    }
    if (err != ESP_OK) {
        printf("Storing the code failed: %s\n", esp_err_to_name(err));
        return 1;
    }
    printf("Code stored; restarting to pair.\n");
    esp_restart();
    return 0;   /* not reached */
}

esp_err_t link_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "link",
        .help = "Show the state of WiFi and of the connection to the server",
        .func = cmd_link,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "link");
    const esp_console_cmd_t pair = {
        .command = "pair",
        .help = "Pair with the server: store the code python -m server pair printed, and restart",
        .hint = "<code>",
        .func = cmd_pair,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&pair), TAG, "pair");
    return ESP_OK;
}
