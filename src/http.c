#include "http.h"
#include "processing.h"
#include "stats.h"
#include <stdarg.h>
#include <stdio.h>
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define LED_GPIO GPIO_NUM_2
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static esp_err_t root_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t led_handler(httpd_req_t *request)
{
    gpio_set_level(LED_GPIO, strcmp(request->uri, "/led/on") == 0);
    httpd_resp_set_status(request, "204 No Content");
    return httpd_resp_send(request, NULL, 0);
}

/* Strings are controlled firmware strings, never request content. */
static bool append_json(char *buffer, size_t capacity, size_t *used, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    int count = vsnprintf(buffer + *used, capacity - *used, format, args);
    va_end(args);
    if (count < 0 || (size_t)count >= capacity - *used) return false;
    *used += count;
    return true;
}

static esp_err_t status_handler(httpd_req_t *request)
{
    processing_status_t state;
    processing_get_status(&state);
    wifi_ap_record_t access_point = {0};
    bool wifi_connected = esp_wifi_sta_get_ap_info(&access_point) == ESP_OK;
    float cpu0 = 0, cpu1 = 0;
    stats_read_cpu_usage(&cpu0, &cpu1);
    float temperature = stats_read_internal_temperature();
    char temp[24] = "null";
    if (isfinite(temperature)) snprintf(temp, sizeof(temp), "%.1f", temperature);
    const size_t capacity = 8192;
    char *json = malloc(capacity);
    if (!json) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Memorie insuficienta");
    size_t used = 0;
    bool ok = append_json(json, capacity, &used,
        "{\"image_width\":%u,\"image_height\":%u,\"uptime_sec\":%llu,\"temperature_c\":%s,"
        "\"cpu0_usage_pct\":%.1f,\"cpu1_usage_pct\":%.1f,"
        "\"internal_ram_free_kb\":%u,\"internal_ram_min_kb\":%u,"
        "\"psram_free_kb\":%u,\"psram_min_kb\":%u,\"wifi_connected\":%s,"
        "\"processing\":{\"busy\":%s,\"has_frame\":%s,\"inferred\":%s,"
        "\"job_id\":%u,\"image_id\":%u,\"threshold\":%.3f,"
        "\"capture_ms\":%.1f,\"inference_ms\":%.1f,\"jpeg_ms\":%.1f,"
        "\"operation\":\"%s\",\"error\":\"%s\",\"total_objects\":%u,\"objects\":[",
        PROCESSING_IMAGE_WIDTH, PROCESSING_IMAGE_HEIGHT,
        (unsigned long long)(esp_timer_get_time() / 1000000), temp, cpu0, cpu1,
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM) / 1024),
        wifi_connected ? "true" : "false", state.busy ? "true" : "false",
        state.has_frame ? "true" : "false", state.inferred ? "true" : "false",
        state.job_id, state.image_id, state.threshold, state.capture_ms,
        state.inference_ms, state.jpeg_ms, state.operation, state.error, state.detections.total);
    for (unsigned i = 0; ok && i < state.detections.count; ++i) {
        const detection_result_t *d = &state.detections.objects[i];
        ok = append_json(json, capacity, &used,
            "%s{\"name\":\"%s\",\"category\":%d,\"score\":%.4f,\"box\":[%d,%d,%d,%d]}",
            i ? "," : "", d->name, d->category, d->score,
            d->box[0], d->box[1], d->box[2], d->box[3]);
    }
    if (ok) ok = append_json(json, capacity, &used, "]}}");
    esp_err_t result;
    if (ok) {
        httpd_resp_set_type(request, "application/json");
        httpd_resp_set_hdr(request, "Cache-Control", "no-store");
        result = httpd_resp_send(request, json, used);
    } else result = httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Raspuns prea mare");
    free(json);
    return result;
}

static esp_err_t submit_handler(httpd_req_t *request)
{
    bool infer = strncmp(request->uri, "/api/infer", 10) == 0;
    float threshold = 0.25f;
    if (infer) {
        char query[64], value[24];
        size_t query_length = httpd_req_get_url_query_len(request);
        if (query_length >= sizeof(query)) return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Parametri prea lungi");
        if (query_length) {
            if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
                httpd_query_key_value(query, "threshold", value, sizeof(value)) != ESP_OK) {
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Prag invalid");
            }
            char *end;
            threshold = strtof(value, &end);
            if (end == value || *end || !isfinite(threshold) || threshold < 0.01f || threshold > 1.0f) {
                return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Pragul trebuie sa fie intre 0.01 si 1");
            }
        }
    }
    esp_err_t err = processing_submit(infer, threshold);
    if (err == ESP_ERR_INVALID_STATE) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Procesare in curs sau nicio fotografie capturata");
    }
    if (err != ESP_OK) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Procesarea nu a putut fi pornita");
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

/* Browser decodes JPEG and fits it to the camera canvas before sending RGB565BE. */
static esp_err_t upload_handler(httpd_req_t *request)
{
    if (request->content_len != PROCESSING_IMAGE_BYTES)
        return httpd_resp_send_err(request, HTTPD_400_BAD_REQUEST, "Imagine RGB565 320x320 necesara");
    processing_status_t status;
    processing_get_status(&status);
    if (status.busy) {
        httpd_resp_set_status(request, "409 Conflict");
        return httpd_resp_sendstr(request, "Procesare in curs");
    }
    uint8_t *pixels = heap_caps_malloc(PROCESSING_IMAGE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!pixels) return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Memorie insuficienta");
    size_t received = 0;
    while (received < PROCESSING_IMAGE_BYTES) {
        int count = httpd_req_recv(request, (char *)pixels + received, PROCESSING_IMAGE_BYTES - received);
        if (count <= 0) {
            free(pixels);
            if (count == HTTPD_SOCK_ERR_TIMEOUT)
                return httpd_resp_send_err(request, HTTPD_408_REQ_TIMEOUT, "Incarcare expirata");
            return ESP_FAIL;
        }
        received += count;
    }
    esp_err_t err = processing_submit_image(pixels, received);
    if (err != ESP_OK) {
        free(pixels);
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(request, "409 Conflict");
            return httpd_resp_sendstr(request, "Procesare in curs");
        }
        return httpd_resp_send_err(request, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload failed");
    }
    httpd_resp_set_status(request, "202 Accepted");
    httpd_resp_set_type(request, "application/json");
    return httpd_resp_sendstr(request, "{\"accepted\":true}");
}

static esp_err_t image_handler(httpd_req_t *request)
{
    uint8_t *jpeg = NULL;
    size_t length = 0;
    esp_err_t err = processing_copy_jpeg(&jpeg, &length);
    if (err != ESP_OK) return httpd_resp_send_err(request,
        err == ESP_ERR_NOT_FOUND ? HTTPD_404_NOT_FOUND : HTTPD_500_INTERNAL_SERVER_ERROR,
        err == ESP_ERR_NOT_FOUND ? "Nicio imagine capturata" : "Memorie insuficienta");
    httpd_resp_set_type(request, "image/jpeg");
    httpd_resp_set_hdr(request, "Cache-Control", "no-store");
    esp_err_t result = httpd_resp_send(request, (const char *)jpeg, length);
    free(jpeg);
    return result;
}

esp_err_t http_server_start(void)
{
    gpio_reset_pin(LED_GPIO);
    gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_level(LED_GPIO, 0);
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    httpd_handle_t server = NULL;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) return result;
    const httpd_uri_t routes[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_handler },
        { .uri = "/led/on", .method = HTTP_POST, .handler = led_handler },
        { .uri = "/led/off", .method = HTTP_POST, .handler = led_handler },
        { .uri = "/api/status", .method = HTTP_GET, .handler = status_handler },
        { .uri = "/api/capture", .method = HTTP_POST, .handler = submit_handler },
        { .uri = "/api/infer", .method = HTTP_POST, .handler = submit_handler },
        { .uri = "/api/upload", .method = HTTP_POST, .handler = upload_handler },
        { .uri = "/image", .method = HTTP_GET, .handler = image_handler },
    };
    for (unsigned i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        result = httpd_register_uri_handler(server, &routes[i]);
        if (result != ESP_OK) { httpd_stop(server); return result; }
    }
    return ESP_OK;
}
