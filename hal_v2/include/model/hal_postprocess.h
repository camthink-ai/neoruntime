/**
 * @file hal_postprocess.h
 * @brief HAL AI postprocessing interface
 * @version 2.0
 *
 * Platform-agnostic postprocessing interface
 * Supports detection, classification, segmentation, keypoint, embedding, OCR (det / rec)
 * Decoupled from inference and drawing
 *
 * OCR uses @c HAL_POST_TYPE_OCR_DETECTION and @c HAL_POST_TYPE_OCR_RECOGNITION with the same
 * Model Zoo / @c hailomz + DFC path to HEF. On Hailo-15, postprocessing defaults to a **built-in**
 * Paddle-style implementation unless JSON sets both @c backend_lib_path and @c backend_function
 * (external @c .so). Inference still attaches output tensors to @c HailoROIPtr (see below).
 *
 * **YOLOv8-Pose (COCO-17):** set @c "native_yolov8_pose": true in the keypoint post JSON to decode in HAL
 * (no @c libyolov8pose_postprocess.so). Optional @c yolov8_pose_network_width / @c yolov8_pose_network_height
 * default to 640.
 *
 * **Monocular depth (SCDepthV3-style):** use @c HAL_POST_TYPE_DEPTH. Hailo-15 decodes the logits tensor in HAL
 * (same math as hailo-apps @c depth_estimation.cpp / @c mono_depth_estimation.cpp); no vendor @c .so is required.
 * Optional JSON: @c scdepth_output_name or @c output_tensor_name (default @c "scdepthv3/conv31"),
 * @c depth_float32 (default false) when the vstream is float32 logits instead of quantized uint8/uint16.
 */

#pragma once

#include "hal_inference.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HAL_MAX_DETECTIONS              256
#define HAL_MAX_CLASSES                 256
#define HAL_MAX_KEYPOINTS               512
#define HAL_MAX_LABEL_LEN               64
#define HAL_MAX_SKELETON_LINKS          128
#define HAL_MAX_EMBEDDING_DIM           512
/** Max length for one CLIP prompt string in HalClipPostprocessConfig (including null). */
#define HAL_MAX_CLIP_PROMPT_LEN         512
/** Max number of negative prompts stored in HalClipPostprocessConfig. */
#define HAL_MAX_CLIP_NEGATIVE_PROMPTS   64

/** Max text lines (regions) returned for @c HAL_POST_TYPE_OCR_RECOGNITION (and merged OCR views). */
#define HAL_MAX_OCR_LINES               64
/** Max UTF-8 bytes per OCR line (excluding null terminator). */
#define HAL_MAX_OCR_TEXT_LEN            512
/** Max path length for OCR charset / frequency-dictionary files in @c HalOcrRecognitionPostConfig. */
#define HAL_MAX_OCR_CONFIG_PATH         512

/* ========== Common Structures ========== */

/* Bounding box (normalized coordinates 0-1) */
typedef struct {
    float x;    // Top-left x
    float y;    // Top-left y
    float w;    // Width
    float h;    // Height
} HalBBox;

/* 2D point */
typedef struct {
    float x;
    float y;
    float confidence;   // Confidence or visibility score
} HalPoint2D;

/* Color */
typedef struct {
    uint8_t r, g, b, a;
} HalColor;

/* ========== Detection Result ========== */

typedef struct {
    HalBBox bbox;
    float confidence;
    int32_t class_id;
    char label[HAL_MAX_LABEL_LEN];
    int32_t track_id;   // Tracking ID, -1 if not available
} HalDetection;

typedef struct {
    uint32_t num_detections;
    HalDetection detections[HAL_MAX_DETECTIONS];
    void *priv;         // Platform-specific data (e.g., HailoROIPtr)
} HalDetectionResult;

/* ========== Classification Result ========== */

typedef struct {
    int32_t class_id;
    /* CLIP (and some other classifiers) may expose a "classification type"
     * string to describe the semantic group / prompt set. */
    char type[HAL_MAX_LABEL_LEN];
    char label[HAL_MAX_LABEL_LEN];
    float confidence;
} HalClassification;

typedef struct {
    uint32_t num_classes;
    HalClassification classes[HAL_MAX_CLASSES];
    int32_t top1_class_id;  // Top-1 class ID
    void *priv;
} HalClassificationResult;

/* ========== Segmentation Result ========== */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t num_classes;
    uint8_t *mask_data;         // HxW, each pixel is class_id
    float *confidence_map;      // HxWxC, confidence per class per pixel (optional)
    void *priv;                 // Platform-specific data for memory management
} HalSegmentationResult;

/* ========== Keypoint Result ========== */

/* Skeleton link definition */
typedef struct {
    int32_t from_idx;   // Start keypoint index
    int32_t to_idx;     // End keypoint index
    HalColor color;     // Line color
    float thickness;    // Line thickness
} HalSkeletonLink;

/* Keypoints for a single object */
typedef struct {
    uint32_t num_keypoints;
    HalPoint2D keypoints[HAL_MAX_KEYPOINTS];
    HalBBox bbox;       // Bounding box of the object (optional)
    float confidence;   // Overall confidence
    int32_t class_id;   // Object class ID
    int32_t track_id;   // Tracking ID, -1 if not available
} HalKeypointObject;

typedef struct {
    uint32_t num_objects;
    HalKeypointObject objects[HAL_MAX_DETECTIONS];

    /* Skeleton definition (shared by all objects) */
    uint32_t num_links;
    HalSkeletonLink links[HAL_MAX_SKELETON_LINKS];

    void *priv;
} HalKeypointResult;

/* ========== Embedding Result ========== */

typedef struct {
    uint32_t dim;
    float data[HAL_MAX_EMBEDDING_DIM];
    void *priv;
} HalEmbeddingResult;

/* ========== OCR Result ========== */

/**
 * One recognized text region. @p bbox uses normalized [0,1] coordinates like @c HalDetection
 * (map to pixels with the same rules as other post types when overlaying on the frame).
 *
 * Vendor post (hailo-apps / hailo-post-processes / suite) is responsible for model-specific
 * decode: e.g. PaddleOCR **detection** maps network outputs to boxes on the ROI; **recognition**
 * + **CTC greedy decode** (e.g. LPRNet logits with blank class) produce the string attached as
 * @c HailoClassification.label. HAL only copies bbox + text + confidence into this struct.
 */
typedef struct {
    HalBBox bbox;
    char text[HAL_MAX_OCR_TEXT_LEN];
    float confidence;
    int32_t track_id; /**< -1 if not available */
} HalOcrLine;

typedef struct {
    uint32_t num_lines;
    HalOcrLine lines[HAL_MAX_OCR_LINES];
    void *priv;
} HalOcrResult;

/** Dense monocular depth map (e.g. SCDepthV3). @c depth_m is row-major, length @c width * @c height. */
typedef struct {
    uint32_t width;
    uint32_t height;
    /** Per-pixel depth in meters (malloc-owned; free with @c HAL_POSTPROCESS_OPS.free_result). */
    float *depth_m;
    void *priv;
} HalDepthResult;

/* ========== Unified Postprocess Result ========== */

typedef enum {
    HAL_POST_TYPE_NONE = 0,
    HAL_POST_TYPE_DETECTION,
    HAL_POST_TYPE_CLASSIFICATION,
    /* Semantic CLIP (zero-shot) matching results.
     * Currently represented using HalClassificationResult (same union member),
     * where label/confidence correspond to matched text and similarity score. */
    HAL_POST_TYPE_CLIP,
    HAL_POST_TYPE_SEGMENTATION,
    HAL_POST_TYPE_KEYPOINT,
    HAL_POST_TYPE_EMBEDDING,
    /**
     * PaddleOCR-style **text detection** (hailo-apps @c ocr_postprocess.cpp, @c libocr_post.so).
     * First-class fields mirror @c local_resources/ocr_config.json; merged into vendor JSON.
     * Hailo-15: if JSON omits @c backend_lib_path / @c backend_function, defaults are
     * @c /usr/lib/hailo-post-processes/libocr_post.so and @c paddleocr_det (same idea as ocr_example_v2).
     * Result uses @c HalDetectionResult (same as @c HAL_POST_TYPE_DETECTION).
     */
    HAL_POST_TYPE_OCR_DETECTION,
    /**
     * PaddleOCR-style **text recognition** (same @c libocr_post.so stack).
     * Hailo-15 default entry when @c backend_* is absent: @c paddleocr_recognize.
     * Result uses @c HalOcrResult in @c HalPostprocessResult.result.ocr.
     */
    HAL_POST_TYPE_OCR_RECOGNITION,
    /** Monocular depth (built-in SCDepthV3-style decode on Hailo-15; see file header). */
    HAL_POST_TYPE_DEPTH,
    HAL_POST_TYPE_CUSTOM = 100,
} HalPostprocessType;

typedef struct {
    HalPostprocessType type;
    union {
        HalDetectionResult detection;
        HalClassificationResult classification;
        HalSegmentationResult segmentation;
        HalKeypointResult keypoint;
        HalEmbeddingResult embedding;
        HalOcrResult ocr;
        HalDepthResult depth;
        void *custom;
    } result;
    void *priv;     // Platform-specific data
} HalPostprocessResult;

/* ========== Dynamic Postprocess Results (optional) ========== */
/**
 * Dynamic result variants avoid embedding the largest HAL result arm (keypoints) into every
 * @c HalPostprocessResult instance via the union. This is useful on embedded targets with small thread stacks.
 *
 * Memory ownership:
 * - Results returned from @c HalPostprocessOps.run_dyn must be released with @c free_result_dyn.
 * - Implementations should allocate with malloc/free-compatible routines (HAL will free).
 */

typedef struct {
    uint32_t num_detections;
    HalDetection *detections; /**< malloc'd array, length = num_detections */
    void *priv;
} HalDetectionResultDyn;

typedef struct {
    uint32_t num_classes;
    HalClassification *classes; /**< malloc'd array, length = num_classes */
    int32_t top1_class_id;
    void *priv;
} HalClassificationResultDyn;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t num_classes;
    uint8_t *mask_data;    /**< malloc'd, size = width*height bytes (class_id per pixel) */
    float *confidence_map; /**< optional malloc'd, size = width*height*num_classes floats */
    void *priv;
} HalSegmentationResultDyn;

typedef struct {
    uint32_t num_keypoints;
    HalPoint2D *keypoints; /**< malloc'd array, length = num_keypoints */
    HalBBox bbox;
    float confidence;
    int32_t class_id;
    int32_t track_id;
} HalKeypointObjectDyn;

typedef struct {
    uint32_t num_objects;
    HalKeypointObjectDyn *objects;  /**< malloc'd array, length = num_objects */
    uint32_t num_links;
    HalSkeletonLink *links;         /**< malloc'd array, length = num_links */
    void *priv;
} HalKeypointResultDyn;

typedef struct {
    uint32_t dim;
    float *data; /**< malloc'd array, length = dim */
    void *priv;
} HalEmbeddingResultDyn;

typedef struct {
    uint32_t num_lines;
    HalOcrLine *lines; /**< malloc'd array, length = num_lines */
    void *priv;
} HalOcrResultDyn;

typedef struct {
    uint32_t width;
    uint32_t height;
    float *depth_m; /**< malloc'd, length = width*height */
    void *priv;
} HalDepthResultDyn;

typedef struct {
    HalPostprocessType type;
    union {
        HalDetectionResultDyn detection;
        HalClassificationResultDyn classification;
        HalSegmentationResultDyn segmentation;
        HalKeypointResultDyn keypoint;
        HalEmbeddingResultDyn embedding;
        HalOcrResultDyn ocr;
        HalDepthResultDyn depth;
        void *custom;
    } result;
    void *priv;
} HalPostprocessResultDyn;

/* ========== Postprocess Config ========== */

/* Detection config */
typedef struct {
    float confidence_threshold;
    float nms_threshold;
    uint32_t max_detections;
    int32_t *class_ids_filter;      // Filter by class IDs, NULL for all
    uint32_t num_class_ids_filter;
    const char *labels_file;        // Path to labels file (optional)
    const char *config_file;        // Platform-specific config file (optional)
    const char *config_json;        // Platform-specific inline JSON/YAML (optional)
} HalDetectionConfig;

/* Classification config */
typedef struct {
    uint32_t top_k;                 // Top-K results
    float confidence_threshold;
    const char *labels_file;
    const char *config_file;
    const char *config_json;
} HalClassificationConfig;

/* Segmentation config */
typedef struct {
    float confidence_threshold;
    uint32_t output_width;          // Output mask width (0 = same as input)
    uint32_t output_height;         // Output mask height (0 = same as input)
    const char *labels_file;
    const char *config_file;
    const char *config_json;
} HalSegmentationConfig;

/* Keypoint config */
typedef struct {
    float confidence_threshold;
    float keypoint_threshold;       // Visibility threshold for individual keypoints
    uint32_t num_keypoints;         // Expected number of keypoints per object
    const char *skeleton_file;      // Skeleton definition file (optional)
    const char *config_file;
    const char *config_json;
} HalKeypointConfig;

/* Embedding config */
typedef struct {
    bool normalize;                 // Normalize embedding vector
    const char *config_file;
    const char *config_json;
} HalEmbeddingConfig;

/**
 * hailo-apps compatible **detection-only** OCR post JSON (see @c ocr_postprocess.cpp schema).
 * Optional @c config_file / @c config_json still pass @c backend_* and patches; when @c config_file
 * is absent, HAL synthesizes a vendor JSON blob from these scalars + strings.
 */
typedef struct {
    float det_bin_thresh;
    float det_box_thresh;
    float det_unclip_ratio;
    int32_t det_max_candidates;
    float det_min_box_size;
    char det_output_name[HAL_MAX_TENSOR_NAME];
    int32_t det_map_h;
    int32_t det_map_w;
    bool letterbox_fix;
    /** Filter boxes after decode (0 = keep all). */
    float min_confidence;
    const char *config_file;
    const char *config_json;
} HalOcrDetectionPostConfig;

/**
 * hailo-apps compatible **recognition** OCR post JSON (charset / CTC flags).
 */
typedef struct {
    char rec_output_name[HAL_MAX_TENSOR_NAME];
    char charset_path[HAL_MAX_OCR_CONFIG_PATH];
    /**
     * Dictionary index offset applied when mapping argmax class index -> charset line.
     * Example: offset=1 means class index 1 maps to charset line 0 (i.e., dict excludes the leading blank token).
     */
    int32_t charset_index_offset;
    int32_t blank_index;
    bool logits_are_softmax;
    bool time_major;
    float text_conf_smooth;
    bool attach_caption_box;
    char frequency_dict_path[HAL_MAX_OCR_CONFIG_PATH];
    int32_t max_edit_distance;
    /** Drop decoded lines below this confidence (0 = keep all). */
    float min_confidence;
    const char *config_file;
    const char *config_json;
} HalOcrRecognitionPostConfig;

/** Depth postprocess (HAL built-in decode; @c config_json holds optional @c scdepth_output_name / @c depth_float32). */
typedef struct {
    const char *config_file;
    const char *config_json;
} HalDepthConfig;

/**
 * CLIP postprocess match policy (application-side and vendor JSON "match_policy").
 * Values align with hal_v2::HalClipMatchPolicy / official clip query path.
 */
typedef enum {
    HAL_CLIP_MATCH_SOFTMAX = 0,
    HAL_CLIP_MATCH_MARGIN = 1,
    HAL_CLIP_MATCH_POS_ONLY = 2,
} HalClipMatchPolicyKind;

/**
 * First-class CLIP postprocess configuration (prefer this over JSON alone).
 * Fields map to vendor / application JSON keys documented below.
 */
typedef struct {
    uint32_t top_k;
    float score_threshold;
    HalClipMatchPolicyKind match_policy;
    /** Optional semantic label for HalClassification.type (JSON key "type"). */
    char semantic_type[HAL_MAX_LABEL_LEN];
    /** Primary prompt for pos/neg scoring (JSON "positive_prompt"). */
    char positive_prompt[HAL_MAX_CLIP_PROMPT_LEN];
    uint32_t num_negative_prompts;
    char negative_prompts[HAL_MAX_CLIP_NEGATIVE_PROMPTS][HAL_MAX_CLIP_PROMPT_LEN];
    /** Multi-prompt zero-shot list (JSON "prompts"); used when positive_prompt is empty. */
    uint32_t num_zero_shot_prompts;
    char zero_shot_prompts[HAL_MAX_CLASSES][HAL_MAX_CLIP_PROMPT_LEN];
    const char *labels_file;
    const char *config_file;
    const char *config_json;
} HalClipPostprocessConfig;

/* Unified config */
typedef struct {
    HalPostprocessType type;
    union {
        HalDetectionConfig detection;
        HalClassificationConfig classification;
        HalClipPostprocessConfig clip;
        HalSegmentationConfig segmentation;
        HalKeypointConfig keypoint;
        HalEmbeddingConfig embedding;
        HalOcrDetectionPostConfig ocr_detection;
        HalOcrRecognitionPostConfig ocr_recognition;
        HalDepthConfig depth;
    } config;
} HalPostprocessConfig;

/* --------------------------------------------------------------------
 * OCR — Model Zoo / DFC / HailoRT / vendor postprocess (@c HAL_POST_TYPE_OCR_DETECTION / @c _RECOGNITION)
 * --------------------------------------------------------------------
 *
 * **Models** — PaddleOCR v5 mobile det/rec HEFs, LPRNet, etc., via **Model Zoo** / @c hailomz + DFC.
 * **License plates** — use @c HAL_POST_TYPE_DETECTION with YOLO post (e.g. @c tiny_yolov4_license_plates) on the full
 *   frame, then @c HAL_POST_TYPE_OCR_RECOGNITION / @c libocr_post.so on each plate crop (see @c hal-lpr-example-v2).
 *
 * **Runtime** — Same as other HAL post types: tensor @c priv carries @c HailoROIPtr after the vendor .so runs.
 *
 * **Vendor plugin** — HAL does not implement CTC or DB decode; @c ocr_postprocess maps tensors to
 * @c HailoDetection / nested @c HailoClassification. @c HalPostprocessConfig.config.ocr_detection /
 * @c ocr_recognition supply hailo-apps JSON fields (or @c config_file / @c config_json for @c backend_*).
 *
 * **HAL mapping (recognition)** — Each @c HailoDetection → one @c HalOcrLine (bbox + text + confidence);
 * root-only classification → one line with bbox @c (0,0,1,1). @c min_confidence filters weak lines.
 */

/* --------------------------------------------------------------------
 * CLIP (HAL_POST_TYPE_CLIP) configuration contract
 * --------------------------------------------------------------------
 *
 * HAL represents CLIP results using HalClassificationResult:
 *   - HalClassification.label      => matched text prompt (or short status line)
 *   - HalClassification.confidence => similarity score (typically cosine similarity after L2 norm)
 *   - HalClassification.type       => optional prompt-set name / semantic group, or "match"/"nomatch"
 *     when applications synthesize results from image embeddings + text encoder scoring.
 *
 * For HAL_POST_TYPE_CLIP, configure via `HalClipPostprocessConfig` fields first; the platform merges them
 * into the vendor JSON blob. Fallback / override:
 * - `HalPostprocessConfig.config.clip.config_file` (path), and/or
 * - `HalPostprocessConfig.config.clip.config_json` (inline string).
 *
 * Precedence: struct fields are applied on top of JSON loaded from config_file/config_json (non-empty struct
 * fields overwrite or append the merged JSON used for `backend_*` and temp config files).
 *
 * Vendor postprocess JSON (backend_lib_path / backend_function / backend_config_path) may embed
 * the following keys alongside Hailo-specific fields. Applications that run **image embedding +
 * HalClipTextEncoder** scoring outside the vendor CLIP plugin use the same keys in `config_json`
 * as a contract for prompts, thresholds, and scoring policy.
 *
 * Extended JSON schema (CLIP and embedding+text scoring):
 *
 * {
 *   "type": "default",              // optional string, copied to HalClassification.type
 *   "top_k": 5,                     // optional, overrides clip.top_k when >0
 *   "score_threshold": 0.3,         // optional; also used as softmax/margin gate (see match_policy)
 *   "prompts": ["a cat", "a dog"],  // optional: multi-prompt zero-shot (vendor CLIP path)
 *   "positive_prompt": "a person", // optional: primary text for embedding+encoder path
 *   "negative_prompts": ["night"],  // optional: array of negative prompts (same path)
 *   "match_policy": "softmax"      // optional: "softmax" | "margin" | "pos_only"
 *                                   // softmax: scale*100 then softmax on [pos, neg...] (Hailo clip app style)
 *                                   // margin: match if pos_dot - max(neg_dots) >= score_threshold
 *                                   // pos_only: match if pos_dot >= score_threshold (cosine / dot on L2-normalized)
 * }
 *
 * When both config_file and config_json are provided, config_file typically has priority for vendor
 * plugin resolution; inline JSON may still describe application-side scoring parameters.
 *
 * Runtime updates: use `HalPostprocessOps.apply_config_json` when implemented (merges scalar/string
 * keys into the session-owned vendor JSON blob and may re-init the plugin if a temp config file is used).
 */

/* ========== Postprocess Session (opaque handle) ========== */
typedef struct HalPostprocessSession HalPostprocessSession;

/* ========== Postprocess Operations ========== */
typedef struct HalPostprocessOps {
    /**
     * @brief Create postprocess session
     * @param config Postprocess configuration
     * @return Session handle, NULL on failure
     */
    HalPostprocessSession* (*create)(const HalPostprocessConfig *config);

    /**
     * @brief Destroy postprocess session
     * @param session Session handle
     */
    void (*destroy)(HalPostprocessSession *session);

    /**
     * @brief Run postprocessing
     * @param session Session handle
     * @param outputs Output tensors from inference
     * @param num_outputs Number of output tensors
     * @param result Output postprocess result
     * @return HAL_OK on success
     */
    int (*run)(HalPostprocessSession *session,
               const HalTensor *outputs, int num_outputs,
               HalPostprocessResult *result);

    /**
     * @brief Free postprocess result
     * @param result Result to free
     */
    void (*free_result)(HalPostprocessResult *result);

    /**
     * @brief Run postprocessing and return a dynamically allocated result (optional API).
     *
     * This avoids large stack frames caused by @c HalPostprocessResult's union. If the backend does not support
     * dynamic results, it may return @c HAL_ERR_NOT_SUPPORTED and callers should fall back to @c run.
     */
    int (*run_dyn)(HalPostprocessSession *session,
                   const HalTensor *outputs, int num_outputs,
                   HalPostprocessResultDyn *result);

    /**
     * @brief Free a dynamic postprocess result returned by @c run_dyn.
     */
    void (*free_result_dyn)(HalPostprocessResultDyn *result);

    /**
     * @brief Get version string
     * @return Version string
     */
    const char* (*get_version)(void);

    /**
     * @brief Merge runtime JSON parameters into the session-owned vendor/config blob and mirror scalars into
     *        HalPostprocessConfig where the session type matches (Hailo-15: may re-init the plugin from temp file).
     *
     * @p patch_json must be a JSON object string. Recognized keys (subset aligned with hailo-analytics JSON such as
     *        apps/webserver/configs/yolov5.json and CLIP post JSON):
     *
     * - HAL_POST_TYPE_DETECTION: `detection_threshold`, `confidence_threshold` (score gate), `iou_threshold`,
     *   `nms_threshold`, `max_boxes`, `max_detections`, `label_offset`, `output_activation` (string, vendor-only).
     * - HAL_POST_TYPE_CLASSIFICATION: `confidence_threshold`, `top_k`.
     * - HAL_POST_TYPE_CLIP: `score_threshold`, `top_k`, `match_policy`, `positive_prompt`, plus detection-like keys
     *   merged into JSON for vendor CLIP plugins that read them.
     * - HAL_POST_TYPE_SEGMENTATION: `confidence_threshold`, `output_width`, `output_height`.
     * - HAL_POST_TYPE_KEYPOINT: `confidence_threshold`, `keypoint_threshold`, `num_keypoints`.
     * - HAL_POST_TYPE_EMBEDDING: `normalize` (bool or 0/1).
     * - HAL_POST_TYPE_OCR_DETECTION / HAL_POST_TYPE_OCR_RECOGNITION: hailo-apps OCR scalars (e.g.
     *   `det_bin_thresh`, …) plus `min_confidence` where applicable.
     * - HAL_POST_TYPE_DEPTH: optional string keys @c scdepth_output_name, @c output_tensor_name; bool @c depth_float32.
     *
     * Only the keys above are read from @p patch_json; other keys are ignored (no generic JSON merge).
     *
     * @return HAL_OK on success, HAL_ERR_INVALID_ARG, HAL_ERR_NOT_SUPPORTED, or HAL_ERR_RESULT if nothing applied
     */
    int (*apply_config_json)(HalPostprocessSession *session, const char *patch_json);
} HalPostprocessOps;

/* ========== Global Operations Table ========== */
extern HalPostprocessOps HAL_POSTPROCESS_OPS;

/* ========== Helper Functions ========== */

/**
 * @brief Initialize detection config with defaults
 */
void hal_detection_config_init(HalDetectionConfig *config);

/**
 * @brief Initialize classification config with defaults
 */
void hal_classification_config_init(HalClassificationConfig *config);

/**
 * @brief Initialize segmentation config with defaults
 */
void hal_segmentation_config_init(HalSegmentationConfig *config);

/**
 * @brief Initialize keypoint config with defaults
 */
void hal_keypoint_config_init(HalKeypointConfig *config);

/**
 * @brief Initialize embedding config with defaults
 */
void hal_embedding_config_init(HalEmbeddingConfig *config);

/**
 * @brief Initialize CLIP postprocess config with defaults (softmax, score_threshold 0.8, top_k 5).
 */
void hal_clip_postprocess_config_init(HalClipPostprocessConfig *config);

/**
 * @brief Initialize OCR **detection** post config (hailo-apps @c ocr_config.json defaults).
 */
void hal_ocr_detection_post_config_init(HalOcrDetectionPostConfig *config);

/**
 * @brief Initialize OCR **recognition** post config (hailo-apps @c ocr_config.json defaults).
 */
void hal_ocr_recognition_post_config_init(HalOcrRecognitionPostConfig *config);

/**
 * @brief Initialize monocular depth post config (optional @c config_file / @c config_json).
 */
void hal_depth_config_init(HalDepthConfig *config);

/**
 * @brief Calculate IoU (Intersection over Union) between two bboxes
 */
float hal_bbox_iou(const HalBBox *a, const HalBBox *b);

/**
 * @brief Non-Maximum Suppression (generic implementation)
 * @param detections Input detections
 * @param num_detections Number of input detections
 * @param nms_threshold IoU threshold
 * @param output Output detections after NMS
 * @param max_output Maximum number of output detections
 * @return Number of output detections
 */
uint32_t hal_nms(const HalDetection *detections, uint32_t num_detections,
                 float nms_threshold, HalDetection *output, uint32_t max_output);

#ifdef __cplusplus
}
#endif
