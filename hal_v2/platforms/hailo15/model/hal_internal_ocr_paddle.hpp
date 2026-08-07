/**
 * @file hal_internal_ocr_paddle.hpp
 * @brief Built-in Paddle-style OCR detection / recognition (no external libocr_post.so).
 *
 * Algorithms aligned with hal_v2/examples/ocr_example_v2 (det: prob-map CC) and
 * hailo-apps ocr_postprocess.cpp (rec: UINT8 logits + CTC greedy + softmax).
 */
#pragma once

#include "model/hal_postprocess.h"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include <hailo_postprocess_tools/objects/hailo_objects.hpp>

#include <string>
#include <vector>

namespace hal_v2::internal_ocr
{

/** Load charset lines from @c cfg.charset_path; if empty, fill Paddle-style default vocabulary. */
void load_recognition_charset(const HalOcrRecognitionPostConfig &cfg, std::vector<std::string> &charset_out);

/**
 * Text detection from UINT8 probability map (full-frame ROI tensor).
 * @return HAL_OK, HAL_ERR_NOT_READY, or HAL_ERR_INVALID_ARG.
 */
int run_detection(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalDetectionResult *out);

/**
 * Recognition: rank-3 UINT8 tensor [1,T,C] or [1,C,T], CTC decode into one line (full-frame bbox).
 */
int run_recognition(const HalPostprocessConfig &cfg, HailoROIPtr roi, const std::vector<std::string> &charset,
                    HalOcrResult *out);

} // namespace hal_v2::internal_ocr

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
