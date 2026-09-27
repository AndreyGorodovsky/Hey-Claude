# Pin map, header only. REQUIRES (not PRIV_): board.h itself includes
# driver/gpio.h, so every user of board.h needs the GPIO driver too.
idf_component_register(INCLUDE_DIRS "include"
                       REQUIRES esp_driver_gpio)
