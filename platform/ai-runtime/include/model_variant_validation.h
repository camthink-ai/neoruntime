#pragma once

#include <string>

namespace aipc::ai_runtime {

/// True when model_type names a post-processing family init_post_process
/// recognizes (mirrors the dispatch in model_manager.cpp: detection/yolo/
/// landmarks/keypoint/segmentation/classification/clip/embedding/depth/
/// monocular_depth/scdepth/ocr_detection/ocr_recognition, compared
/// case-insensitively). The empty string is NOT "known" — it means "no
/// post-processing" and callers accept it before consulting this function.
bool is_known_model_type(const std::string& model_type);

/// True when name is one of the vendor plugin's exported detection
/// backend_function symbols. Mirrors platform-api's
/// DetectionPostprocessProfiles (platform/platform-api/models/model_types.go)
/// — NOT hal_v2's 3-entry kFamilyFunctions heuristic (that one only drives a
/// load-time warning). Keep the Go side and this list in sync.
bool is_known_detection_backend(const std::string& name);

/// Registration-time validation of (model_type, model_variant) for the gRPC
/// RegisterModel surface, mirroring the REST boundary rules
/// (platform/platform-api/handlers/ai_postprocess.go) so an SDK-side typo
/// cannot register a model whose post-processing silently decodes nothing:
///  - detection/yolo: empty OK; a JSON blob must contain exactly the closed
///    7-key detection schema with backend_function whitelisted; a bare name
///    must itself be a whitelisted backend_function.
///  - landmarks/keypoint: empty OK; a JSON blob OK (the plugin validates its
///    content); a bare backend_function name is refused — it cannot select
///    the COCO-17 pose decoder and silently keeps the facial-landmarks
///    default.
///  - any other known type: pass-through (blast radius limited to the two
///    families where the silent-no-op trap lives).
/// Returns "" when the pair is acceptable for registration, otherwise a
/// human-readable refusal reason for the RegisterModel status message.
std::string validate_model_variant(const std::string& model_type,
                                   const std::string& variant);

}  // namespace aipc::ai_runtime
