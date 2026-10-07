# The WebSocket connection to the server. The WebSocket client and mDNS come
# from idf_component.yml.
idf_component_register(SRCS "server_link.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES protocol         # the protocol's numbers
                                     esp_event        # WiFi's events
                                     esp_netif        # address types and printing
                                     esp_wifi         # WiFi's disconnect event
                                     json)            # cJSON, for the server's messages
