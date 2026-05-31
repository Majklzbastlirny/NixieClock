idf_component_register(
    SRCS "mqttctrl.c"
    INCLUDE_DIRS "include"
    REQUIRES settings netclock antipoison temps alarm control netcfg rtcdev common mqtt esp_wifi esp_netif esp_timer
)
