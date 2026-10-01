# The capture task and the ring buffer that shares the microphone's audio.
idf_component_register(SRCS "audio_ring.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES audio
                                     esp_psram)       # the ring lives in PSRAM
