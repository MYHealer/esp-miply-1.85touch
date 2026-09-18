idf_component_register(
    SRCS "wifi_provision.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi esp_netif esp_http_server nvs_flash
    PRIV_REQUIRES esp_system lwip freertos
)
