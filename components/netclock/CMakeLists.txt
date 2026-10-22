idf_component_register(
    SRCS "netclock.c"
    INCLUDE_DIRS "include"
    REQUIRES rtcdev netcfg settings esp_wifi esp_netif esp_event nvs_flash lwip esp_timer
)
