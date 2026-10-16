#include "processing.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "img_converters.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { bool infer; float threshold; uint8_t *pixels; } job_t;
static QueueHandle_t jobs;
static SemaphoreHandle_t lock;
static processing_status_t state;
/* Raw pixels belong exclusively to the worker. The camera buffer is returned
   immediately after copying; inference always uses a separate scratch copy. */
static camera_fb_t saved;
static uint8_t *latest_jpeg;
static size_t latest_length;

static void *pixel_alloc(size_t length)
{
    return heap_caps_malloc(length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static void worker(void *arg)
{
    job_t job;
    for (;;) {
        if (xQueueReceive(jobs, &job, portMAX_DELAY) != pdTRUE) continue;
        processing_status_t next;
        processing_get_status(&next);
        next.error[0] = 0;
        next.inference_ms = 0;
        next.jpeg_ms = 0;
        next.detections = (detection_results_t){0};
        camera_fb_t work = {0};
        uint8_t *jpeg = NULL;
        size_t length = 0;
        esp_err_t err = ESP_OK;
        int64_t start = esp_timer_get_time();
        if (job.pixels) {
            work = (camera_fb_t){ .buf = job.pixels, .len = PROCESSING_IMAGE_BYTES,
                .width = PROCESSING_IMAGE_WIDTH, .height = PROCESSING_IMAGE_HEIGHT,
                .format = PIXFORMAT_RGB565 };
            next.capture_ms = 0;
        } else if (!job.infer) {
            next.capture_ms = 0;
            camera_fb_t *frame = esp_camera_fb_get();
            if (frame) { esp_camera_fb_return(frame); frame = esp_camera_fb_get(); }
            if (!frame) {
                err = ESP_FAIL;
                snprintf(next.error, sizeof(next.error), "Camera capture failed");
            } else {
                work = *frame;
                work.buf = pixel_alloc(frame->len);
                if (work.buf) memcpy(work.buf, frame->buf, frame->len);
                esp_camera_fb_return(frame);
                if (!work.buf) err = ESP_ERR_NO_MEM;
            }
            next.capture_ms = (esp_timer_get_time() - start) / 1000.0f;
        } else {
            work = saved;
            work.buf = pixel_alloc(saved.len);
            if (!work.buf) err = ESP_ERR_NO_MEM;
            else {
                memcpy(work.buf, saved.buf, saved.len);
                start = esp_timer_get_time();
                err = detection_run(&work, job.threshold, &next.detections);
                next.inference_ms = (esp_timer_get_time() - start) / 1000.0f;
                if (err == ESP_OK) detection_draw(&work, &next.detections);
                else snprintf(next.error, sizeof(next.error), "Detection failed: %s", esp_err_to_name(err));
            }
        }
        if (err == ESP_OK) {
            start = esp_timer_get_time();
            if (!frame2jpg(&work, 80, &jpeg, &length)) {
                err = ESP_FAIL;
                snprintf(next.error, sizeof(next.error), "JPEG conversion failed");
            }
            next.jpeg_ms = (esp_timer_get_time() - start) / 1000.0f;
        }
        if (err != ESP_OK && !next.error[0]) {
            snprintf(next.error, sizeof(next.error), "Processing failed: %s", esp_err_to_name(err));
        }
        xSemaphoreTake(lock, portMAX_DELAY);
        if (err == ESP_OK) {
            free(latest_jpeg);
            latest_jpeg = jpeg;
            latest_length = length;
            jpeg = NULL;
            if (!job.infer) {
                free(saved.buf);
                saved = work;
                work.buf = NULL;
            }
            next.has_frame = true;
            next.inferred = job.infer;
            next.image_id++;
        } else {
            /* Keep the previous image and its results on failure. */
            next.detections = state.detections;
        }
        next.busy = false;
        state = next;
        xSemaphoreGive(lock);
        free(work.buf);
        free(jpeg);
    }
}

esp_err_t processing_init(void)
{
    lock = xSemaphoreCreateMutex();
    jobs = xQueueCreate(1, sizeof(job_t));
    if (!lock || !jobs) {
        if (lock) vSemaphoreDelete(lock);
        if (jobs) vQueueDelete(jobs);
        return ESP_ERR_NO_MEM;
    }
    state.threshold = 0.25f;
    /* Detection blocks briefly between model stages, so it can run above
       idle priority without starving the watchdog's idle tasks. HTTP/Wi-Fi
       and the periodic statistics task still have higher priority. */
    if (xTaskCreate(worker, "image_worker", 16384, NULL, tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        vSemaphoreDelete(lock);
        vQueueDelete(jobs);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t processing_submit(bool infer, float threshold)
{
    if (!isfinite(threshold) || threshold < 0.01f || threshold > 1.0f) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state.busy || (infer && !state.has_frame)) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    processing_status_t previous = state;
    state.busy = true;
    state.job_id++;
    state.threshold = threshold;
    state.error[0] = 0;
    snprintf(state.operation, sizeof(state.operation), "%s", infer ? "inference" : "capture");
    job_t job = { .infer = infer, .threshold = threshold };
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        state = previous;
        xSemaphoreGive(lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

esp_err_t processing_submit_image(uint8_t *pixels, size_t length)
{
    if (!pixels || length != PROCESSING_IMAGE_BYTES) return ESP_ERR_INVALID_ARG;
    xSemaphoreTake(lock, portMAX_DELAY);
    if (state.busy) {
        xSemaphoreGive(lock);
        return ESP_ERR_INVALID_STATE;
    }
    processing_status_t previous = state;
    state.busy = true;
    state.job_id++;
    state.error[0] = 0;
    snprintf(state.operation, sizeof(state.operation), "upload");
    job_t job = { .pixels = pixels, .threshold = state.threshold };
    if (xQueueSend(jobs, &job, 0) != pdTRUE) {
        state = previous;
        xSemaphoreGive(lock);
        return ESP_FAIL;
    }
    xSemaphoreGive(lock);
    return ESP_OK;
}

void processing_get_status(processing_status_t *status)
{
    xSemaphoreTake(lock, portMAX_DELAY);
    *status = state;
    xSemaphoreGive(lock);
}

esp_err_t processing_copy_jpeg(uint8_t **data, size_t *length)
{
    *data = NULL;
    *length = 0;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (latest_jpeg) {
        *data = pixel_alloc(latest_length);
        if (*data) {
            memcpy(*data, latest_jpeg, latest_length);
            *length = latest_length;
            err = ESP_OK;
        } else err = ESP_ERR_NO_MEM;
    }
    xSemaphoreGive(lock);
    return err;
}
