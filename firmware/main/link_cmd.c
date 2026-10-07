/*
 * Console command for the server connection.
 *
 *   link        whether WiFi and the server connection are up, where the
 *               server was found, and how often the connection was made
 *
 * For telling apart "the device cannot find the server" from "the server
 * does not answer" when the screen shows CONNECTING or ERROR.
 *
 * Context: the handler runs in the console task.
 */
#include "link_cmd.h"

#include <stdio.h>
#include "esp_check.h"
#include "esp_console.h"
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

esp_err_t link_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "link",
        .help = "Show the state of WiFi and of the connection to the server",
        .func = cmd_link,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "link");
    return ESP_OK;
}
