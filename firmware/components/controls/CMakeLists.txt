# The push button and the slide switch on the breadboard (controls.h).
# REQUIRES (not PRIV_) esp_event: controls.h declares its event base.
idf_component_register(SRCS "controls.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event
                       PRIV_REQUIRES board esp_driver_gpio esp_timer esp_rom log)
