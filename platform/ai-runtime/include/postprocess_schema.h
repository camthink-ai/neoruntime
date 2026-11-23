// GENERATED FILE — DO NOT EDIT.
// Single source of truth: platform/postprocess (registry.go).
// Regenerate with: make postprocess-schema  (go generate ./platform/postprocess)
// CI fails when this file drifts from the registry.
#ifndef AI_RUNTIME_POSTPROCESS_SCHEMA_H_
#define AI_RUNTIME_POSTPROCESS_SCHEMA_H_

#include <cstddef>

namespace aipc::ai_runtime::postprocess_schema {

// Model type strings the gRPC RegisterModel validator accepts, verbatim
// from the registry's RuntimeTypes. An unknown type falls through to the
// silent detection default in init_post_process — membership changes are
// paired with that switch and a regeneration.
inline constexpr const char* const kModelTypes[] = {
    "detection",
    "yolo",
    "landmarks",
    "keypoint",
    "segmentation",
    "classification",
    "clip",
    "embedding",
    "depth",
    "monocular_depth",
    "scdepth",
    "ocr_detection",
    "ocr_recognition",
};
inline constexpr std::size_t kModelTypesCount = sizeof(kModelTypes) / sizeof(kModelTypes[0]);

// Detection backend_function whitelist (parameterized plugin entries plus
// the fixed-name customer entry).
inline constexpr const char* const kDetectionBackends[] = {
    "hailo_yolov8n",
    "hailo_yolov8s",
    "hailo_yolov8m",
    "yolov5m_vehicles",
};
inline constexpr std::size_t kDetectionBackendsCount = sizeof(kDetectionBackends) / sizeof(kDetectionBackends[0]);

// The closed detection variant key schema.
inline constexpr const char* const kDetectionVariantKeys[] = {
    "backend_function",
    "iou_threshold",
    "detection_threshold",
    "output_activation",
    "label_offset",
    "max_boxes",
    "labels",
};
inline constexpr std::size_t kDetectionVariantKeysCount = sizeof(kDetectionVariantKeys) / sizeof(kDetectionVariantKeys[0]);
inline constexpr const char* kDetectionSchemaList = "backend_function, iou_threshold, detection_threshold, output_activation, label_offset, max_boxes, labels";

// Informational keypoint key set — the dialect itself is OPEN (legacy
// opaque channel; HAL validates content); only the loader keys in
// kForbiddenVariantKeys are refused on every surface.
inline constexpr const char* const kKeypointVariantKeys[] = {
    "native_yolov8_pose",
    "score_threshold",
    "confidence_threshold",
    "keypoint_threshold",
    "iou_threshold",
    "num_keypoints",
    "yolov8_pose_network_width",
    "yolov8_pose_network_height",
};
inline constexpr std::size_t kKeypointVariantKeysCount = sizeof(kKeypointVariantKeys) / sizeof(kKeypointVariantKeys[0]);

// Loader control keys the HAL postprocess layer dlopens from — rejected on
// every variant surface regardless of model type.
inline constexpr const char* const kForbiddenVariantKeys[] = {
    "backend_lib_path",
    "backend_config_path",
};
inline constexpr std::size_t kForbiddenVariantKeysCount = sizeof(kForbiddenVariantKeys) / sizeof(kForbiddenVariantKeys[0]);

// Full decoder matrix. Numeric min/max/default are rendered as strings —
// C++ consumers need membership checks, not arithmetic; the JSON artifact
// carries the exact typed values for tooling.
struct ParamSpec {
    const char* key;
    const char* type;
    const char* min;            // "" = unbounded
    const char* max;            // "" = unbounded
    const char* default_value;  // "" = none
    const char* mutability;
    const char* effect;
    const char* dialect_key;    // "" = same spelling as key
    const char* note;
};

struct HardcodedSpec {
    const char* key;
    const char* value;
    const char* note;
};

struct DecoderSpec {
    const char* model_type;
    const char* decoder;
    const char* label;
    const char* select_value;  // "" = implicit single-decoder default
    const char* basename;      // detection plugin basenames only, else ""
    const char* selection;     // routing mechanism, see the registry
    bool custom;
    const ParamSpec* params;
    std::size_t params_count;
    const HardcodedSpec* hardcoded;
    std::size_t hardcoded_count;
};

inline constexpr ParamSpec kParams_detection_hailo_yolov8n[] = {
    {"backend_function", "string", "", "", "hailo_yolov8n", "create_only", "consumed", "", ""},
    {"labels", "string_list", "", "", "", "create_only", "consumed", "", ""},
    {"detection_threshold", "number", "0", "1", "0.25", "hot_update", "consumed", "", ""},
    {"max_boxes", "integer", "1", "999", "64", "create_only", "consumed", "", ""},
    {"iou_threshold", "number", "0", "1", "0.45", "hot_update", "advisory", "", ""},
    {"output_activation", "string", "", "", "none", "create_only", "advisory", "", ""},
    {"label_offset", "integer", "", "", "1", "create_only", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_detection_hailo_yolov8s[] = {
    {"backend_function", "string", "", "", "hailo_yolov8s", "create_only", "consumed", "", ""},
    {"labels", "string_list", "", "", "", "create_only", "consumed", "", ""},
    {"detection_threshold", "number", "0", "1", "0.25", "hot_update", "consumed", "", ""},
    {"max_boxes", "integer", "1", "999", "64", "create_only", "consumed", "", ""},
    {"iou_threshold", "number", "0", "1", "0.45", "hot_update", "advisory", "", ""},
    {"output_activation", "string", "", "", "none", "create_only", "advisory", "", ""},
    {"label_offset", "integer", "", "", "1", "create_only", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_detection_hailo_yolov8m[] = {
    {"backend_function", "string", "", "", "hailo_yolov8m", "create_only", "consumed", "", ""},
    {"labels", "string_list", "", "", "", "create_only", "consumed", "", ""},
    {"detection_threshold", "number", "0", "1", "0.25", "hot_update", "consumed", "", ""},
    {"max_boxes", "integer", "1", "999", "64", "create_only", "consumed", "", ""},
    {"iou_threshold", "number", "0", "1", "0.45", "hot_update", "advisory", "", ""},
    {"output_activation", "string", "", "", "none", "create_only", "advisory", "", ""},
    {"label_offset", "integer", "", "", "1", "create_only", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_detection_yolov5m_vehicles[] = {
    {"backend_function", "string", "", "", "yolov5m_vehicles", "create_only", "consumed", "", ""},
    {"labels", "string_list", "", "", "", "create_only", "advisory", "", ""},
    {"detection_threshold", "number", "0", "1", "0.25", "hot_update", "advisory", "", ""},
    {"max_boxes", "integer", "1", "999", "64", "create_only", "advisory", "", ""},
    {"iou_threshold", "number", "0", "1", "0.45", "hot_update", "advisory", "", ""},
    {"output_activation", "string", "", "", "none", "create_only", "advisory", "", ""},
    {"label_offset", "integer", "", "", "1", "create_only", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_keypoint_native_yolov8_pose[] = {
    {"native_yolov8_pose", "boolean", "", "", "true", "create_only", "consumed", "", ""},
    {"score_threshold", "number", "0.01", "1", "0.25", "hot_update", "consumed", "", "takes precedence over confidence_threshold; <1e-6 silently falls back to 0.6 in the decoder"},
    {"keypoint_threshold", "number", "0", "1", "0.25", "hot_update", "consumed", "", ""},
    {"confidence_threshold", "number", "0", "1", "0.25", "hot_update", "consumed", "", "only used when score_threshold is absent"},
    {"iou_threshold", "number", "0", "1", "0.7", "hot_update", "consumed", "", "adopted only when 0 < v < 1"},
    {"yolov8_pose_network_width", "integer", "16", "4096", "640", "create_only", "consumed", "", "read once at create; absent from both hot-update key tables"},
    {"yolov8_pose_network_height", "integer", "16", "4096", "640", "create_only", "consumed", "", "read once at create; absent from both hot-update key tables"},
    {"num_keypoints", "number", "", "", "", "hot_update", "advisory", "", "decoder hardcodes COCO-17; this key changes nothing"},
};

inline constexpr ParamSpec kParams_segmentation_linknet[] = {
    {"threshold", "number", "0", "1", "0.25", "hot_update", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_clip_clip_zeroshot[] = {
    {"score_threshold", "number", "0", "1", "", "hot_update", "advisory", "", ""},
    {"top_k", "number", "1", "100", "5", "hot_update", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_ocr_detection_paddle_det[] = {
    {"threshold", "number", "0", "1", "0.25", "hot_update", "advisory", "", ""},
    {"max_detections", "number", "1", "999", "64", "create_only", "advisory", "", ""},
};

inline constexpr ParamSpec kParams_scdepth_scdepth[] = {
    {"scdepth_output_name", "string", "", "", "", "create_only", "consumed", "", ""},
    {"depth_float32", "boolean", "", "", "", "create_only", "consumed", "", ""},
};

inline constexpr HardcodedSpec kHardcoded_detection_hailo_yolov8n[] = {
    {"output_tensor", "hailo_yolov8n_384_640/yolov8_nms_postprocess", ""},
    {"labels", "COCO 80", "compiled-in table; the labels key only relabels for consumers"},
    {"filter_by_score", "true", ""},
};

inline constexpr HardcodedSpec kHardcoded_detection_hailo_yolov8s[] = {
    {"output_tensor", "hailo_yolov8s_384_640/yolov8_nms_postprocess", ""},
    {"labels", "COCO 80", "compiled-in table; the labels key only relabels for consumers"},
    {"filter_by_score", "true", ""},
};

inline constexpr HardcodedSpec kHardcoded_detection_hailo_yolov8m[] = {
    {"output_tensor", "hailo_yolov8m_384_640/yolov8_nms_postprocess", ""},
    {"labels", "COCO 80", "compiled-in table; the labels key only relabels for consumers"},
    {"filter_by_score", "true", ""},
};

inline constexpr HardcodedSpec kHardcoded_detection_yolov5m_vehicles[] = {
    {"output_tensor", "yolov5m_vehicles/yolov5_nms_postprocess", ""},
    {"labels", "\"car\" (single class, baked)", ""},
};

inline constexpr HardcodedSpec kHardcoded_keypoint_facial_landmarks_nv12[] = {
    {"num_keypoints", "468", ""},
    {"presence_threshold", "0.5", ""},
    {"input_normalization", "192x192", ""},
    {"output_tensors", "face_landmarks_lite/conv22, face_landmarks_lite/conv25", ""},
};

inline constexpr HardcodedSpec kHardcoded_keypoint_native_yolov8_pose[] = {
    {"num_keypoints", "17 (COCO-17 topology)", ""},
};

inline constexpr HardcodedSpec kHardcoded_segmentation_linknet[] = {
    {"mask_threshold", "0.5", ""},
    {"output_layout", "single channel, one class per output tensor", ""},
};

inline constexpr HardcodedSpec kHardcoded_clip_clip_zeroshot[] = {
    {"logit_scale", "100", ""},
    {"prompt_prefix", "\"A photo of \"", ""},
    {"top_k", "not implemented (returns all scores)", ""},
    {"prompts_channel", "ZeroMQ runtime messages, not the config file", ""},
};

inline constexpr HardcodedSpec kHardcoded_embedding_clipgen[] = {
    {"embedding_dims", "640 / 512 / 768 (per model)", ""},
    {"output_layout", "single tensor, 1-D assumed", ""},
};

inline constexpr HardcodedSpec kHardcoded_ocr_recognition_paddle_rec[] = {
    {"charset", "compiled-in table", ""},
};

inline constexpr DecoderSpec kDecoders[] = {
    {"detection", "hailo_yolov8n", "YOLOv8n 384x640 (default)", "hailo_yolov8n_384_640", "hailo_yolov8n_384_640", "backend_function", false, kParams_detection_hailo_yolov8n, 7, kHardcoded_detection_hailo_yolov8n, 3},
    {"detection", "hailo_yolov8s", "YOLOv8s 384x640", "hailo_yolov8s_384_640", "hailo_yolov8s_384_640", "backend_function", false, kParams_detection_hailo_yolov8s, 7, kHardcoded_detection_hailo_yolov8s, 3},
    {"detection", "hailo_yolov8m", "YOLOv8m 384x640", "hailo_yolov8m_384_640", "hailo_yolov8m_384_640", "backend_function", false, kParams_detection_hailo_yolov8m, 7, kHardcoded_detection_hailo_yolov8m, 3},
    {"detection", "yolov5m_vehicles", "YOLOv5m Vehicles 1920x1080", "yolov5m_vehicles", "yolov5m_vehicles", "backend_function", true, kParams_detection_yolov5m_vehicles, 7, kHardcoded_detection_yolov5m_vehicles, 2},
    {"keypoint", "facial_landmarks_nv12", "Face Landmarks (default)", "facial_landmarks", "", "", false, nullptr, 0, kHardcoded_keypoint_facial_landmarks_nv12, 4},
    {"keypoint", "native_yolov8_pose", "YOLOv8 Pose (COCO-17)", "yolov8_pose", "", "flag:native_yolov8_pose", false, kParams_keypoint_native_yolov8_pose, 8, kHardcoded_keypoint_native_yolov8_pose, 1},
    {"segmentation", "linknet", "LinkNet Segmentation", "", "", "", false, kParams_segmentation_linknet, 1, kHardcoded_segmentation_linknet, 2},
    {"clip", "clip_zeroshot", "CLIP Zero-Shot Classification", "", "", "", false, kParams_clip_clip_zeroshot, 2, kHardcoded_clip_clip_zeroshot, 4},
    {"embedding", "clipgen", "CLIP Image Embedding", "", "", "", false, nullptr, 0, kHardcoded_embedding_clipgen, 2},
    {"ocr_detection", "paddle_det", "PaddleOCR Text Detection", "", "", "", false, kParams_ocr_detection_paddle_det, 2, nullptr, 0},
    {"ocr_recognition", "paddle_rec", "PaddleOCR Text Recognition", "", "", "", false, nullptr, 0, kHardcoded_ocr_recognition_paddle_rec, 1},
    {"depth", "monocular_depth", "Monocular Depth", "", "", "", false, nullptr, 0, nullptr, 0},
    {"scdepth", "scdepth", "SCDepth", "", "", "", false, kParams_scdepth_scdepth, 2, nullptr, 0},
};
inline constexpr std::size_t kDecodersCount = sizeof(kDecoders) / sizeof(kDecoders[0]);

}  // namespace aipc::ai_runtime::postprocess_schema

#endif  // AI_RUNTIME_POSTPROCESS_SCHEMA_H_
