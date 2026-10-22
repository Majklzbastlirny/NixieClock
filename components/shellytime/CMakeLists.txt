idf_component_register(
    SRCS "shellytime.c"
    INCLUDE_DIRS "include"
    REQUIRES settings netclock rtcdev esp_timer
)
