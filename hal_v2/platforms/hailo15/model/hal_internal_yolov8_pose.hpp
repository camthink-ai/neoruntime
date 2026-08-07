/**
 * @file hal_internal_yolov8_pose.hpp
 * @brief Built-in YOLOv8-Pose decode (no libyolov8pose_postprocess.so / hailo-apps).
 *
 * Enable with vendor JSON @c "native_yolov8_pose": true on @c HAL_POST_TYPE_KEYPOINT.
 * Algorithm and tensor layout follow hailo_apps @c yolov8pose_postprocess.cpp and
 * @c common/tensors.hpp (@c get_xtensor / @c HailoTensor indexing). ROI tensors are regrouped by
 * (H,W) and channel counts because @c HailoROI::get_tensors() iterates an internal name-ordered map.
 */
#pragma once

#include "model/hal_postprocess.h"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include <hailo_postprocess_tools/objects/hailo_objects.hpp>

namespace hal_v2::internal_yolov8_pose
{

/** Decode ROI tensors (box/score/kpt triplets per stride) into @p out. */
int run(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalKeypointResult *out);

} // namespace hal_v2::internal_yolov8_pose

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
