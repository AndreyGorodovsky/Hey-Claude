idf_component_register(SRCS "main.c" "console.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_config net console nvs_flash esp_event esp_netif
                                     esp_app_format spi_flash esp_psram esp_system espcoredump)
