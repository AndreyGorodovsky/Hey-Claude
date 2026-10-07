# Reply playback: the buffer for reply audio and the task that plays it,
# and the chime that the same task plays when the wake word is heard.
idf_component_register(SRCS "player.c" "chime.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio
                                     esp_psram)       # the buffer lives in PSRAM
