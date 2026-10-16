#include "detection.h"
#include "coco_detect.hpp"
#include "dl_image_draw.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <list>
#include <new>
#include <vector>

static const char *TAG = "detection";
/* Preserve the component's preprocessing/postprocessing, but execute its
   model one layer at a time so a long inference can periodically block and
   let IDLE feed the watchdog. No managed component files are modified. */
class YieldingYolo : public coco_detect::Yolo11n {
public:
    YieldingYolo() : coco_detect::Yolo11n("coco_detect_yolo11n_320_s8_v1.espdl",
                                        default_score_thr, default_nms_thr)
    {
        m_stage_count = static_cast<int>(m_model->get_fbs_model()->topological_sort().size());
    }

    bool ready() const { return m_stage_count > 0; }

    std::list<dl::detect::result_t> &run(const dl::image::img_t &image) override
    {
        m_image_preprocessor->preprocess(image);
        vTaskDelay(1);
        int64_t last_pause = esp_timer_get_time();
        int64_t longest_stage_us = 0;
        unsigned pauses = 0;
        for (int stage = 0; stage < m_stage_count; ++stage) {
            const int64_t start = esp_timer_get_time();
            m_model->run(m_stage_count, stage, dl::RUNTIME_MODE_SINGLE_CORE);
            const int64_t end = esp_timer_get_time();
            longest_stage_us = std::max(longest_stage_us, end - start);
            if (end - last_pause >= WORK_BUDGET_US) {
                vTaskDelay(1);
                ++pauses;
                last_pause = esp_timer_get_time();
            }
        }
        vTaskDelay(1);
        m_postprocessor->clear_result();
        m_postprocessor->postprocess();
        vTaskDelay(1);
        ESP_LOGI(TAG, "Inference: %d stages, %u yields, longest layer %.1f ms",
                 m_stage_count, pauses, longest_stage_us / 1000.0);
        return m_postprocessor->get_result(image.width, image.height);
    }

private:
    static constexpr int64_t WORK_BUDGET_US = 25000;
    int m_stage_count = 0;
};

static YieldingYolo *detector;

static const char *const COCO_CLASSES[] = {
    "person",       "bicycle",      "car",           "motorcycle",   "airplane",
    "bus",          "train",        "truck",         "boat",         "traffic light",
    "fire hydrant", "stop sign",    "parking meter", "bench",        "bird",
    "cat",          "dog",          "horse",         "sheep",        "cow",
    "elephant",     "bear",         "zebra",         "giraffe",      "backpack",
    "umbrella",     "handbag",      "tie",           "suitcase",     "frisbee",
    "skis",         "snowboard",    "sports ball",   "kite",         "baseball bat",
    "baseball glove", "skateboard", "surfboard",     "tennis racket", "bottle",
    "wine glass",   "cup",          "fork",          "knife",        "spoon",
    "bowl",         "banana",       "apple",         "sandwich",     "orange",
    "broccoli",     "carrot",       "hot dog",       "pizza",        "donut",
    "cake",         "chair",        "couch",         "potted plant", "bed",
    "dining table", "toilet",       "tv",            "laptop",       "mouse",
    "remote",       "keyboard",     "cell phone",    "microwave",    "oven",
    "toaster",      "sink",         "refrigerator",  "book",         "clock",
    "vase",         "scissors",     "teddy bear",    "hair drier",   "toothbrush",
};

static const char *category_name(int category)
{
    const int category_count = sizeof(COCO_CLASSES) / sizeof(COCO_CLASSES[0]);
    return category >= 0 && category < category_count ? COCO_CLASSES[category] : "unknown";
}

extern "C" esp_err_t detection_init(void)
{
    if (detector != nullptr) {
        return ESP_OK;
    }

    detector = new (std::nothrow) YieldingYolo();
    if (detector == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate COCO model");
        return ESP_ERR_NO_MEM;
    }

    if (!detector->ready()) {
        ESP_LOGE(TAG, "COCO model has no executable stages");
        delete detector;
        detector = nullptr;
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "COCO Detect YOLO11N 320 initialized with yields between stages");
    return ESP_OK;
}

extern "C" esp_err_t detection_run(camera_fb_t *frame, float threshold,
                                    detection_results_t *output)
{
    if (detector == nullptr || frame == nullptr || frame->buf == nullptr || output == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame->format != PIXFORMAT_RGB565 || threshold < 0.01f || threshold > 1.0f) {
        return ESP_ERR_INVALID_ARG;
    }
    *output = {};
    dl::image::img_t image = {
        .data = frame->buf,
        .width = static_cast<uint16_t>(frame->width),
        .height = static_cast<uint16_t>(frame->height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };
    detector->set_score_thr(threshold, 0);
    auto &results = detector->run(image);
    output->total = results.size();
    for (const auto &result : results) {
        if (result.box.size() < 4 || output->count == DETECTION_MAX_RESULTS) continue;
        auto &item = output->objects[output->count++];
        item.name = category_name(result.category);
        item.category = result.category;
        item.score = result.score;
        for (int i = 0; i < 4; ++i) {
            int limit = (i % 2 == 0 ? frame->width : frame->height) - 1;
            item.box[i] = std::max(0, std::min(result.box[i], limit));
        }
    }
    ESP_LOGI(TAG, "Objects detected: %u", output->total);
    return ESP_OK;
}

extern "C" void detection_draw(camera_fb_t *frame, const detection_results_t *results)
{
    dl::image::img_t image = {
        .data = frame->buf,
        .width = static_cast<uint16_t>(frame->width),
        .height = static_cast<uint16_t>(frame->height),
        .pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE,
    };
    const std::vector<uint8_t> color = {0x07, 0xe0};
    for (unsigned i = 0; i < results->count; ++i) {
        const int *b = results->objects[i].box;
        if (b[2] > b[0] && b[3] > b[1]) {
            dl::image::draw_hollow_rectangle(image, b[0], b[1], b[2], b[3], color, 3);
        }
    }
}
