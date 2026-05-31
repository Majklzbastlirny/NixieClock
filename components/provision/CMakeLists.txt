idf_component_register(
    SRCS "provision.c"
    INCLUDE_DIRS "include"
    REQUIRES netcfg common nixie esp_wifi esp_netif esp_event esp_http_server esp_timer nvs_flash lwip driver
)
