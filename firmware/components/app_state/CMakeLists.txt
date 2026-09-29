# The device's state and its change announcements. REQUIRES (not PRIV_):
# app_state.h includes esp_event.h, so every user of the header needs it too.
idf_component_register(SRCS "app_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_event)
