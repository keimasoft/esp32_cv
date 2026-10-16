idf_component_register(
    SRCS main.c camera.c detection.cpp processing.c http.c wifi.c stats.c
    EMBED_TXTFILES index.html
    PRIV_REQUIRES esp_driver_gpio esp_driver_tsens esp_http_server esp_netif esp_timer
                  esp_psram esp_wifi heap log nvs_flash spi_flash
)
