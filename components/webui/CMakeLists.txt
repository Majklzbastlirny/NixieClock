idf_component_register(
    SRCS "webui.c"
    INCLUDE_DIRS "include"
    REQUIRES control antipoison alarm temps netcfg mqttctrl esp_http_server app_update nixie weblog blescan settings rtcdev
)
