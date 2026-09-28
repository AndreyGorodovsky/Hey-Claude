# Microphone capture and amplifier playback over I2S.
idf_component_register(SRCS "audio.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES board
                                     esp_driver_i2s   # I2S controllers
                                     esp_driver_gpio) # amplifier SD pin
