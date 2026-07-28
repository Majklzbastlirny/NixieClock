idf_component_register(
    SRCS "mqttctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES settings netclock antipoison temps alarm control netcfg rtcdev common bh1750 mqtt esp_wifi esp_netif esp_timer
)
