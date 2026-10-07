# The display: panel, backlight, LVGL and what is drawn.
idf_component_register(SRCS "display.c" "display_ui.c" "display_test.c"
                       INCLUDE_DIRS "include"
                       REQUIRES app_state           # display.h uses its state type
                       PRIV_REQUIRES board
                                     lvgl             # graphics library, from idf_component.yml
                                     esp_lcd          # panel IO over SPI
                                     esp_driver_spi   # SPI bus
                                     esp_driver_gpio  # reset pin
                                     esp_driver_ledc  # backlight PWM
                                     esp_timer        # LVGL's clock, transfer timing
                                     esp_psram)       # frame buffer in PSRAM
