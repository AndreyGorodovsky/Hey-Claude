/*
 * Console command for the playback volume.
 *
 *   volume          print the volume, from 1 to 10
 *   volume <level>  set it, as "volume five" said to the device would
 *
 * In daily use the volume is changed by voice: the server recognises
 * "louder", "quieter" or "volume five" and sends the device its new level.
 * This command is for the bench, where a level is wanted without speaking.
 * Either way the state machine makes the change (state_machine.h), which
 * applies it from the next reply or chime and keeps it in NVS through a
 * restart.
 *
 * Context: the handler runs in the console task.
 */
#include "volume_cmd.h"

#include <stdio.h>
#include "console.h"
#include "esp_check.h"
#include "esp_console.h"
#include "protocol.h"
#include "state_machine.h"

static const char *TAG = "volume_cmd";

static int cmd_volume(int argc, char **argv)
{
    int level = 0;
    if (argc > 2 || (argc == 2 && !console_parse_int(argv[1], PROTO_VOLUME_MIN,
                                                     PROTO_VOLUME_MAX, &level))) {
        printf("usage: volume [%d-%d]\n", PROTO_VOLUME_MIN, PROTO_VOLUME_MAX);
        return 1;
    }
    if (argc == 2) {
        esp_err_t err = state_machine_set_volume((unsigned)level);
        if (err == ESP_ERR_INVALID_STATE) {
            /* The state machine starts only once WiFi and the token are set */
            printf("the volume can be set once the device is set up\n");
            return 1;
        }
        if (err != ESP_OK) {
            printf("volume not set: %s\n", esp_err_to_name(err));
            return 1;
        }
        printf("volume %d, from the next reply or chime\n", level);
        return 0;
    }
    unsigned now = state_machine_volume();
    if (now == 0) {
        printf("the volume is loaded once the device is set up\n");
        return 1;
    }
    printf("volume %u of %d\n", now, PROTO_VOLUME_MAX);
    return 0;
}

esp_err_t volume_cmd_register(void)
{
    const esp_console_cmd_t cmd = {
        .command = "volume",
        .help = "Show the playback volume, or set it: volume [1-10]",
        .func = cmd_volume,
    };
    ESP_RETURN_ON_ERROR(esp_console_cmd_register(&cmd), TAG, "volume");
    return ESP_OK;
}
