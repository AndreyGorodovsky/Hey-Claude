# The state machine, the request upload it runs, and the playback volume
# it keeps. REQUIRES (not PRIV_) for link and player: state_machine.h uses
# their types in the functions it offers them. nvs_flash: the volume is
# kept in NVS.
idf_component_register(SRCS "state_machine.c" "upload.c" "volume.c"
                       INCLUDE_DIRS "include"
                       REQUIRES server_link player
                       PRIV_REQUIRES app_state wakeword audio_ring protocol nvs_flash
                                     esp_event
                                     controls)  # the button that interrupts a turn
