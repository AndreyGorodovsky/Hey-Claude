# Settings storage. PRIV_REQUIRES: used inside this component only, not
# exposed to code that includes app_config.h.
idf_component_register(SRCS "app_config.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES nvs_flash        # NVS storage
                                     esp_hw_support)  # MAC address for the default device ID
