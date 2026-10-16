#pragma once
#include "detection.h"
#include "esp_err.h"
#include <stdbool.h>

#define PROCESSING_IMAGE_WIDTH 320
#define PROCESSING_IMAGE_HEIGHT 320
#define PROCESSING_IMAGE_BYTES (PROCESSING_IMAGE_WIDTH * PROCESSING_IMAGE_HEIGHT * 2)

typedef struct {
    bool busy;
    bool has_frame;
    bool inferred;
    unsigned job_id;
    unsigned image_id;
    float threshold;
    float capture_ms;
    float inference_ms;
    float jpeg_ms;
    char operation[16];
    char error[96];
    detection_results_t detections;
} processing_status_t;

esp_err_t processing_init(void);
esp_err_t processing_submit(bool infer, float threshold);
void processing_get_status(processing_status_t *status);
esp_err_t processing_copy_jpeg(uint8_t **data, size_t *length);

/* Takes ownership of pixels only on success. RGB565 big endian, fixed size. */
esp_err_t processing_submit_image(uint8_t *pixels, size_t length);
