/* Face detection wrapper */
#include "face_detect.h"
#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "face_detect";

#if __has_include("human_face_detect.hpp")
#define HAS_FACE_DETECT 1
#include "human_face_detect.hpp"
#include "dl_image_define.hpp"
#else
#define HAS_FACE_DETECT 0
#endif

#if HAS_FACE_DETECT
static HumanFaceDetect *s_detector = nullptr;
#endif

static bool s_initialized = false;
static int s_frame_w = 640;
static int s_frame_h = 480;

extern "C" {

bool face_detect_init(int frame_w, int frame_h)
{
    s_frame_w = frame_w;
    s_frame_h = frame_h;
#if HAS_FACE_DETECT
    if (s_detector) return true;
    ESP_LOGI(TAG, "Creating HumanFaceDetect (PICO 224)...");
    s_detector = new HumanFaceDetect(
        HumanFaceDetect::ESPDET_PICO_224_224_FACE, false);
    if (!s_detector) {
        ESP_LOGE(TAG, "new HumanFaceDetect returned NULL");
        return false;
    }
    s_initialized = true;
    ESP_LOGI(TAG, "Face detection ready (%dx%d) model=%d", frame_w, frame_h,
        (int)CONFIG_DEFAULT_HUMAN_FACE_DETECT_MODEL);
    return true;
#else
    s_initialized = false;
    ESP_LOGW(TAG, "human_face_detect not available");
    return false;
#endif
}

int face_detect_run(const uint8_t *uyvy, int w, int h,
                    face_box_t *boxes, int max_boxes)
{
    (void)uyvy; (void)w; (void)h; (void)boxes; (void)max_boxes;
#if HAS_FACE_DETECT
    if (!s_initialized || !s_detector || !uyvy) return 0;

    /* Wrap UYVY buffer as dl::image::img_t — zero copy */
    dl::image::img_t img;
    img.data = (void *)uyvy;
    img.width = w;
    img.height = h;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_UYVY;

    /* Run detection */
    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> &results = s_detector->run(img);
    int64_t t1 = esp_timer_get_time();

    /* Find the largest face by box area */
    int best_area = 0;
    face_box_t best;
    memset(&best, 0, sizeof(best));
    for (const auto &r : results) {
        int ww = r.box[2] - r.box[0];
        int hh = r.box[3] - r.box[1];
        int area = ww * hh;
        if (area > best_area) {
            best_area = area;
            best.x = r.box[0];
            best.y = r.box[1];
            best.w = ww;
            best.h = hh;
            best.confidence = r.score;
        }
    }
    if (best_area > 0 && boxes) {
        *boxes = best;
        ESP_LOGI(TAG, "Detect: %dms raw=%zu largest=[%d,%d,%d,%d] area=%d",
            (int)((t1 - t0) / 1000), results.size(),
            best.x, best.y, best.w, best.h, best_area);
        return 1;
    }
    ESP_LOGI(TAG, "Detect: %dms raw=%zu faces=0", (int)((t1 - t0) / 1000), results.size());
    return 0;
#else
    return 0;
#endif
}

void face_draw_boxes(uint8_t *uyvy, int w, int h,
                     const face_box_t *boxes, int count)
{
    for (int i = 0; i < count; i++) {
        int x0 = boxes[i].x, y0 = boxes[i].y;
        int x1 = x0 + boxes[i].w, y1 = y0 + boxes[i].h;
        if (x0 < 0) x0 = 0;
        if (x1 >= w) x1 = w - 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= h) y1 = h - 1;

        /* White box (Y=235, U=V=128 neutral) — simpler and guaranteed visible */
        for (int y = y0; y <= y1; y += (y == y0 || y == y1 ? 1 : y1 - y0)) {
            for (int x = x0; x <= x1; x++) {
                int idx = (y * w + x) * 2;
                if (x & 1) { uyvy[idx - 1] = 128; uyvy[idx] = 235; }
                else       { uyvy[idx] = 128; uyvy[idx + 1] = 235; }
            }
        }
        for (int y = y0 + 1; y < y1; y++) {
            for (int x = x0; x <= x1; x += (x == x0 || x == x1 ? 1 : x1 - x0)) {
                int idx = (y * w + x) * 2;
                if (x & 1) { uyvy[idx - 1] = 128; uyvy[idx] = 235; }
                else       { uyvy[idx] = 128; uyvy[idx + 1] = 235; }
            }
        }
    }
}

} /* extern "C" */

