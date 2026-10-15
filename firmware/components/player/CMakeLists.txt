# Reply playback: the buffer for reply audio and the task that plays it,
# the chime that the same task plays when the wake word is heard, and the
# playback level with its limiter.
idf_component_register(SRCS "player.c" "chime.c" "gain.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio
                                     esp_psram)       # the buffer lives in PSRAM
