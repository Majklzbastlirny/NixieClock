idf_component_register(
    SRCS "i2cbus.c"
    INCLUDE_DIRS "include"
    REQUIRES common driver esp_driver_i2c
)
