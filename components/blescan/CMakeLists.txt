idf_component_register(
    SRCS "blescan.c"
    INCLUDE_DIRS "include"
    REQUIRES settings temps shellytime bt esp_timer
)
