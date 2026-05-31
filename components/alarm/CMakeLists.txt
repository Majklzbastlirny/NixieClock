idf_component_register(
    SRCS "alarm.c"
    INCLUDE_DIRS "include"
    REQUIRES settings buzzer esp_timer
)
