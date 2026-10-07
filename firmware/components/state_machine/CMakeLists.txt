# The state machine and the request upload it runs. REQUIRES (not PRIV_) for
# link and player: state_machine.h uses their types in the functions it offers them.
idf_component_register(SRCS "state_machine.c" "upload.c"
                       INCLUDE_DIRS "include"
                       REQUIRES server_link player
                       PRIV_REQUIRES app_state wakeword audio_ring protocol
                                     esp_event)
