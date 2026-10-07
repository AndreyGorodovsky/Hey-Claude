# The `main` component: startup and the serial console.
# PRIV_REQUIRES lists the components this code uses. Under MINIMAL_BUILD a
# component missing from this list is not built at all.
idf_component_register(SRCS "main.c" "console.c" "audio_cmd.c" "ui_cmd.c"
                            "wake_cmd.c" "link_cmd.c" "temp_cmd.c" "mem_cmd.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES app_config net audio audio_ring app_state display
                                     wakeword server_link player state_machine protocol
                                     console          # serial command line
                                     nvs_flash        # settings storage
                                     esp_event        # default event loop
                                     esp_netif        # TCP/IP layer
                                     esp_app_format   # version in the boot banner
                                     spi_flash        # flash size in the boot banner
                                     esp_psram        # PSRAM support
                                     esp_system       # reset reason, restart
                                     esp_driver_tsens # chip temperature (temp_cmd.c)
                                     espcoredump)     # crash dump to flash
