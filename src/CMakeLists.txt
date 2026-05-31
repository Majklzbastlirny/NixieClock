idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES common nixie rtcdev i2cbus netclock antipoison settings mqttctrl temps buttons alarm buzzer control webui provision netcfg esp_wifi esp_netif
)
