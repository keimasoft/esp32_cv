#include "camera.h"
#include "detection.h"
#include "http.h"
#include "nvs_flash.h"
#include "stats.h"
#include "processing.h"
#include "wifi.h"

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_result = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_result);

    ESP_ERROR_CHECK(stats_init());
    stats_print_system_info();

    ESP_ERROR_CHECK(camera_init());
    ESP_ERROR_CHECK(detection_init());
    ESP_ERROR_CHECK(processing_init());

    wifi_station_start_cv();
    ESP_ERROR_CHECK(http_server_start());
}
