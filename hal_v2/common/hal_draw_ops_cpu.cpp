/**
 * @file hal_draw_ops_cpu.cpp
 * @brief Default HAL_DRAW_OPS: pure CPU rasterization (see hal_draw_cpu.cpp).
 */

#include "model/hal_draw.h"

extern "C" {

extern int hal_draw_cpu_draw_detection(const HalDetectionResult *r,
                                       HalFrameBuffer *f,
                                       const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_classification(const HalClassificationResult *r,
                                            HalFrameBuffer *f,
                                            const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_segmentation(const HalSegmentationResult *r,
                                          HalFrameBuffer *f,
                                          const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_keypoint(const HalKeypointResult *r,
                                      HalFrameBuffer *f,
                                      const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_result(const HalPostprocessResult *r,
                                    HalFrameBuffer *f,
                                    const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_rect(HalFrameBuffer *f, const HalDrawRect *r);
extern int hal_draw_cpu_draw_line(HalFrameBuffer *f, const HalDrawLine *l);
extern int hal_draw_cpu_draw_circle(HalFrameBuffer *f, const HalDrawCircle *c);
extern int hal_draw_cpu_draw_text(HalFrameBuffer *f, const HalDrawText *t);
extern int hal_draw_cpu_draw_polygon(HalFrameBuffer *f, const HalDrawPolygon *p);
extern int hal_draw_cpu_draw_mask(HalFrameBuffer *f, const HalDrawMask *m);
extern int hal_draw_cpu_draw_mosaic(HalFrameBuffer *f, const HalDrawMosaic *m);
extern const char *hal_draw_cpu_get_version(void);
}

HalDrawOps HAL_DRAW_OPS = {
    .draw_detection_result = hal_draw_cpu_draw_detection,
    .draw_classification_result = hal_draw_cpu_draw_classification,
    .draw_segmentation_result = hal_draw_cpu_draw_segmentation,
    .draw_keypoint_result = hal_draw_cpu_draw_keypoint,
    .draw_result = hal_draw_cpu_draw_result,
    .draw_rect = hal_draw_cpu_draw_rect,
    .draw_line = hal_draw_cpu_draw_line,
    .draw_circle = hal_draw_cpu_draw_circle,
    .draw_text = hal_draw_cpu_draw_text,
    .draw_polygon = hal_draw_cpu_draw_polygon,
    .draw_mask = hal_draw_cpu_draw_mask,
    .draw_mosaic = hal_draw_cpu_draw_mosaic,
    .get_version = hal_draw_cpu_get_version,
};
