
idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    PRIV_REQUIRES
        driver
        esp_wifi
        esp_event
        esp_netif
        esp_http_client
        mbedtls
        nvs_flash
        esp_timer
        esp_rom
)
