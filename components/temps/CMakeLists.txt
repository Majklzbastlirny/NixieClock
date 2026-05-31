idf_component_register(
    SRCS "temps.c"
    INCLUDE_DIRS "include"
    REQUIRES settings common esp_timer driver
)
