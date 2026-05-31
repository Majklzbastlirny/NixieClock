idf_component_register(
    SRCS "nixie.c"
    INCLUDE_DIRS "include"
    REQUIRES common driver esp_timer
)
