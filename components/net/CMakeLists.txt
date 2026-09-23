idf_component_register(
    SRCS "net.c"
    INCLUDE_DIRS "include"
    REQUIRES bus nvs_flash esp_wifi esp_netif esp_event esp_timer freertos
)
