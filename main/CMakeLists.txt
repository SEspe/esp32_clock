idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES nvs_flash esp_wifi esp_event esp_netif driver lwip esp_driver_i2c esp_http_server app_update esp_timer
)
