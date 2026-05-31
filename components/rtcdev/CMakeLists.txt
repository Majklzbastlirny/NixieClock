idf_component_register(
    SRCS "rtc.c"
    INCLUDE_DIRS "include"
    REQUIRES i2cbus common esp_driver_i2c
)
