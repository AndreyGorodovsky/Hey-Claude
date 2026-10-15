# WiFi station component.
idf_component_register(SRCS "net.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_wifi esp_netif esp_event
                                     esp_timer        # retry and give-up timers
                                     nvs_flash        # whether the settings ever worked
                                     esp_rom)         # their checksum
