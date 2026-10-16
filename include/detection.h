#ifndef DETECTION_H
#define DETECTION_H

#include "esp_camera.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t detection_init(void);
#define DETECTION_MAX_RESULTS 32

typedef struct {
    const char *name;
    int category;
    float score;
    int box[4];
} detection_result_t;

typedef struct {
    unsigned count;
    unsigned total;
    detection_result_t objects[DETECTION_MAX_RESULTS];
} detection_results_t;

esp_err_t detection_run(camera_fb_t *frame, float threshold, detection_results_t *output);
void detection_draw(camera_fb_t *frame, const detection_results_t *results);

#ifdef __cplusplus
}
#endif

#endif
