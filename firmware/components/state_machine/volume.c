/*
 * The playback volume. See volume.h for what it is and who calls what.
 */
#include "volume.h"

#include <stdatomic.h>
#include <stdint.h>
#include "esp_log.h"
#include "nvs.h"
#include "player.h"
#include "protocol.h"

static const char *TAG = "volume";

/* NVS names are limited to 15 characters */
static const char *NVS_NAMESPACE = "volume";
static const char *NVS_KEY = "level";

/* The level at first boot, before anyone has changed it. It plays replies
 * exactly as the server sends them (0 dB), as every stage before this one
 * did; the scale is built outward from it. */
#define LEVEL_DEFAULT       8
/* Decibels between one level and the next. 3 dB is about the smallest
 * change heard clearly as louder or quieter. With the default at 0 dB, the
 * scale runs from +6 dB at the top to -21 dB at the bottom. */
#define STEP_DB             3

_Static_assert(LEVEL_DEFAULT >= PROTO_VOLUME_MIN && LEVEL_DEFAULT <= PROTO_VOLUME_MAX,
               "the default must be on the scale");
_Static_assert(STEP_DB * (PROTO_VOLUME_MAX - LEVEL_DEFAULT) <= PLAYER_GAIN_MAX_DB,
               "the top of the scale must be within what the player can boost");

/* atomic: written by the state machine's task, read from any */
static atomic_uint s_level;
/* The level last stored, or 0 if it is not known to be stored. Used only
 * by the state machine's task. */
static unsigned s_stored;

bool volume_on_scale(unsigned level)
{
    return level >= PROTO_VOLUME_MIN && level <= PROTO_VOLUME_MAX;
}

int volume_db(unsigned level)
{
    return STEP_DB * ((int)level - LEVEL_DEFAULT);
}

unsigned volume_level(void)
{
    return atomic_load(&s_level);
}

void volume_set(unsigned level)
{
    unsigned was = atomic_load(&s_level);
    if (level == was) {
        return;
    }
    ESP_LOGI(TAG, "volume %u to %u (%d dB)", was, level, volume_db(level));
    atomic_store(&s_level, level);
    player_set_gain_db(volume_db(level));
}

void volume_init(void)
{
    uint8_t stored = 0;
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err == ESP_OK) {
        err = nvs_get_u8(h, NVS_KEY, &stored);
        nvs_close(h);
    }
    /* NOT_FOUND, from either call, only means nothing was stored yet */
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "volume not read: %s", esp_err_to_name(err));
    }
    /* A stored level off the scale, which only a change to the scale could
     * leave, gets the default too */
    unsigned level = (err == ESP_OK && volume_on_scale(stored)) ? stored : LEVEL_DEFAULT;
    s_stored = err == ESP_OK ? stored : 0;
    atomic_store(&s_level, level);
    player_set_gain_db(volume_db(level));
    ESP_LOGI(TAG, "volume %u (%d dB)", level, volume_db(level));
}

void volume_store(void)
{
    unsigned level = atomic_load(&s_level);
    if (level == s_stored) {
        return;     /* nothing new, and no flash to wear */
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY, (uint8_t)level);
        if (err == ESP_OK) {
            /* NVS may hold a write in RAM until committed */
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err == ESP_OK) {
        s_stored = level;
    } else {
        /* It still applies until the next restart. Not tried again until
         * the level changes or the device next returns to IDLE. */
        ESP_LOGE(TAG, "volume not stored: %s", esp_err_to_name(err));
    }
}
