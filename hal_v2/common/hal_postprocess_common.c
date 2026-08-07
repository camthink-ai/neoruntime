/**
 * @file hal_postprocess_common.c
 * @brief Common helpers for HAL postprocessing (config init, IoU, NMS).
 */

#include "model/hal_postprocess.h"
#include "common/hal_common.h"

#include <string.h>

void hal_detection_config_init(HalDetectionConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->confidence_threshold = 0.25f;
    config->nms_threshold = 0.45f;
    config->max_detections = HAL_MAX_DETECTIONS;
    config->class_ids_filter = NULL;
    config->num_class_ids_filter = 0;
    config->labels_file = NULL;
    config->config_file = NULL;
}

void hal_classification_config_init(HalClassificationConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->top_k = 5;
    config->confidence_threshold = 0.0f;
    config->labels_file = NULL;
    config->config_file = NULL;
}

void hal_segmentation_config_init(HalSegmentationConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->confidence_threshold = 0.0f;
    config->output_width = 0;
    config->output_height = 0;
    config->labels_file = NULL;
    config->config_file = NULL;
}

void hal_keypoint_config_init(HalKeypointConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->confidence_threshold = 0.25f;
    config->keypoint_threshold = 0.25f;
    config->num_keypoints = 0;
    config->skeleton_file = NULL;
    config->config_file = NULL;
}

void hal_embedding_config_init(HalEmbeddingConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->normalize = true;
    config->config_file = NULL;
}

void hal_clip_postprocess_config_init(HalClipPostprocessConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->top_k = 5U;
    config->score_threshold = 0.8f;
    config->match_policy = HAL_CLIP_MATCH_SOFTMAX;
}

void hal_ocr_detection_post_config_init(HalOcrDetectionPostConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    /* Defaults aligned with hal_v2/doc/hailo-apps/local_resources/ocr_config.json */
    config->det_bin_thresh = 0.3f;
    config->det_box_thresh = 0.15f;
    config->det_unclip_ratio = 3.0f;
    config->det_max_candidates = 100;
    config->det_min_box_size = 1.0f;
    config->det_map_h = 960;
    config->det_map_w = 544;
    config->letterbox_fix = true;
    config->min_confidence = 0.0f;
    config->config_file = NULL;
    config->config_json = NULL;
}

void hal_depth_config_init(HalDepthConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
}

void hal_ocr_recognition_post_config_init(HalOcrRecognitionPostConfig *config)
{
    if (!config)
        return;
    memset(config, 0, sizeof(*config));
    config->charset_index_offset = 0;
    config->blank_index = 0;
    config->logits_are_softmax = false;
    config->time_major = false;
    config->text_conf_smooth = 0.9f;
    config->attach_caption_box = false;
    config->max_edit_distance = 2;
    config->min_confidence = 0.0f;
    config->config_file = NULL;
    config->config_json = NULL;
}

float hal_bbox_iou(const HalBBox *a, const HalBBox *b)
{
    if (!a || !b)
        return 0.0f;
    const float ax2 = a->x + a->w;
    const float ay2 = a->y + a->h;
    const float bx2 = b->x + b->w;
    const float by2 = b->y + b->h;

    const float ix1 = (a->x > b->x) ? a->x : b->x;
    const float iy1 = (a->y > b->y) ? a->y : b->y;
    const float ix2 = (ax2 < bx2) ? ax2 : bx2;
    const float iy2 = (ay2 < by2) ? ay2 : by2;

    const float iw = ix2 - ix1;
    const float ih = iy2 - iy1;
    if (iw <= 0.0f || ih <= 0.0f)
        return 0.0f;

    const float inter = iw * ih;
    const float area_a = (a->w > 0.0f && a->h > 0.0f) ? (a->w * a->h) : 0.0f;
    const float area_b = (b->w > 0.0f && b->h > 0.0f) ? (b->w * b->h) : 0.0f;
    const float uni = area_a + area_b - inter;
    if (uni <= 0.0f)
        return 0.0f;
    return inter / uni;
}

static void swap_det(HalDetection *a, HalDetection *b)
{
    HalDetection tmp = *a;
    *a = *b;
    *b = tmp;
}

static void sort_by_confidence_desc(HalDetection *dets, uint32_t n)
{
    // Simple selection sort; n is capped by HAL_MAX_DETECTIONS.
    for (uint32_t i = 0; i < n; i++)
    {
        uint32_t best = i;
        for (uint32_t j = i + 1; j < n; j++)
        {
            if (dets[j].confidence > dets[best].confidence)
                best = j;
        }
        if (best != i)
            swap_det(&dets[i], &dets[best]);
    }
}

uint32_t hal_nms(const HalDetection *detections, uint32_t num_detections,
                 float nms_threshold, HalDetection *output, uint32_t max_output)
{
    if (!detections || !output || max_output == 0)
        return 0;
    if (num_detections == 0)
        return 0;

    HalDetection tmp[HAL_MAX_DETECTIONS];
    uint32_t n = num_detections;
    if (n > HAL_MAX_DETECTIONS)
        n = HAL_MAX_DETECTIONS;
    memcpy(tmp, detections, sizeof(HalDetection) * n);
    sort_by_confidence_desc(tmp, n);

    bool suppressed[HAL_MAX_DETECTIONS];
    memset(suppressed, 0, sizeof(suppressed));

    uint32_t out_n = 0;
    for (uint32_t i = 0; i < n && out_n < max_output; i++)
    {
        if (suppressed[i])
            continue;
        output[out_n++] = tmp[i];
        for (uint32_t j = i + 1; j < n; j++)
        {
            if (suppressed[j])
                continue;
            const float iou = hal_bbox_iou(&tmp[i].bbox, &tmp[j].bbox);
            if (iou >= nms_threshold)
            {
                suppressed[j] = true;
            }
        }
    }
    return out_n;
}

