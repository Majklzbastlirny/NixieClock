idf_component_register(
    SRCS "control.c"
    INCLUDE_DIRS "include"
    REQUIRES settings temps alarm netclock bh1750 esp_wifi esp_netif app_update esp_app_format
)
