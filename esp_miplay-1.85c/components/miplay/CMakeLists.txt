idf_component_register(
    SRCS "miplay.c"
    INCLUDE_DIRS "include"
    REQUIRES mdns lwip esp_wifi esp_netif esp_timer mbedtls nvs_flash
             esp_driver_gpio esp_ringbuf lvgl_port
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-unused-function)
