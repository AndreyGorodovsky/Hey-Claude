# Setup mode: the hotspot, the settings page, the captive-portal DNS, and
# the BOOT button that opens it. Its own way of starting up (setup.h).
idf_component_register(SRCS "setup.c" "provision.c" "captive_dns.c" "button.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES app_config app_state display board
                                     esp_wifi esp_netif esp_event
                                     esp_http_server    # the settings page
                                     esp_driver_gpio    # the BOOT button
                                     esp_timer lwip)
