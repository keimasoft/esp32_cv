#include "stats.h"
#include "driver/temperature_sensor.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"

static const char *TAG = "stats";

#include "freertos/task.h"
#include <math.h>

static temperature_sensor_handle_t temperature_sensor;
static portMUX_TYPE sample_lock = portMUX_INITIALIZER_UNLOCKED;
static float latest_temperature = NAN;
static float latest_cpu[2];

static void sample_task(void *arg)
{
    configRUN_TIME_COUNTER_TYPE previous_idle[2] = {
        ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)
    };
    int64_t previous_time = esp_timer_get_time();
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(1000));
        int64_t now = esp_timer_get_time();
        configRUN_TIME_COUNTER_TYPE idle[2] = {
            ulTaskGetIdleRunTimeCounterForCore(0), ulTaskGetIdleRunTimeCounterForCore(1)
        };
        float cpu[2] = {0};
        for (int i = 0; i < 2; ++i) {
            if (now > previous_time) {
                cpu[i] = 100.0f - 100.0f * (float)(idle[i] - previous_idle[i]) / (float)(now - previous_time);
                cpu[i] = fminf(100.0f, fmaxf(0.0f, cpu[i]));
            }
            previous_idle[i] = idle[i];
        }
        previous_time = now;
        float temperature = NAN;
        if (temperature_sensor && temperature_sensor_get_celsius(temperature_sensor, &temperature) != ESP_OK) temperature = NAN;
        portENTER_CRITICAL(&sample_lock);
        latest_cpu[0] = cpu[0]; latest_cpu[1] = cpu[1];
        latest_temperature = temperature;
        portEXIT_CRITICAL(&sample_lock);
    }
}

esp_err_t stats_init(void)
{
    temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    esp_err_t err = temperature_sensor_install(&config, &temperature_sensor);
    if (err == ESP_OK) err = temperature_sensor_enable(temperature_sensor);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Temperature sensor unavailable: %s", esp_err_to_name(err));
        if (temperature_sensor) temperature_sensor_uninstall(temperature_sensor);
        temperature_sensor = NULL;
    } else {
        float initial = NAN;
        if (temperature_sensor_get_celsius(temperature_sensor, &initial) == ESP_OK) latest_temperature = initial;
    }
    if (xTaskCreate(sample_task, "stats", 3072, NULL, 2, NULL) != pdPASS) {
        if (temperature_sensor) {
            temperature_sensor_disable(temperature_sensor);
            temperature_sensor_uninstall(temperature_sensor);
            temperature_sensor = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

float stats_read_internal_temperature(void)
{
    portENTER_CRITICAL(&sample_lock);
    float value = latest_temperature;
    portEXIT_CRITICAL(&sample_lock);
    return value;
}

void stats_read_cpu_usage(float *cpu0_usage, float *cpu1_usage)
{
    portENTER_CRITICAL(&sample_lock);
    if (cpu0_usage) *cpu0_usage = latest_cpu[0];
    if (cpu1_usage) *cpu1_usage = latest_cpu[1];
    portEXIT_CRITICAL(&sample_lock);
}

void stats_print_system_info(void)
{
    const size_t internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    const size_t internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    ESP_LOGI(TAG, "Internal RAM: total=%u KB, free=%u KB, used=%u KB",
             (unsigned)(internal_total / 1024),
             (unsigned)(internal_free / 1024),
             (unsigned)((internal_total - internal_free) / 1024));

    if (esp_psram_is_initialized()) {
        const size_t psram_total = esp_psram_get_size();
        const size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);

        ESP_LOGI(TAG, "PSRAM initialized: total=%u KB, free=%u KB, used=%u KB",
                 (unsigned)(psram_total / 1024),
                 (unsigned)(psram_free / 1024),
                 (unsigned)((psram_total - psram_free) / 1024));
    } else {
        ESP_LOGW(TAG, "PSRAM is not initialized");
    }

    uint32_t flash_size = 0;
    if (esp_flash_get_size(NULL, &flash_size) == ESP_OK) {
        ESP_LOGI(TAG, "Flash detected: %u KB", (unsigned)(flash_size / 1024));
    } else {
        ESP_LOGW(TAG, "Failed to read flash size");
    }

    const float temperature = stats_read_internal_temperature();
    if (isfinite(temperature)) {
        ESP_LOGI(TAG, "ESP32-S3 internal temperature: %.1f C", temperature);
    } else {
        ESP_LOGW(TAG, "Failed to read internal temperature");
    }
}
