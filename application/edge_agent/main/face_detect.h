/* Face detection wrapper — C-compatible API for esp-who C++ internals */
#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_DETECT_MAX_FACES 10

typedef struct {
    int x, y, w, h;       /* bounding box in frame coordinates */
    float confidence;
} face_box_t;

/* Init detector. Call once before any detection. Returns true on success. */
bool face_detect_init(int frame_w, int frame_h);

/* Run face detection on a UYVY frame.
   Returns number of faces found, fills 'boxes' array (max FACE_DETECT_MAX_FACES).
   'boxes' and 'count' may be NULL to just run detection for side effects. */
int face_detect_run(const uint8_t *uyvy, int w, int h,
                    face_box_t *boxes, int max_boxes);

/* Draw colored bounding boxes directly onto a UYVY frame */
void face_draw_boxes(uint8_t *uyvy, int w, int h,
                     const face_box_t *boxes, int count);

#ifdef __cplusplus
}
#endif
