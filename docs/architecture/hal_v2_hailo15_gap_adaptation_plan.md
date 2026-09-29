# HAL v2 Hailo-15 Capability Gap Adaptation Plan

> Status: implemented (v1.1, 2026-08-31 drafted; **M1 / M2 / M3 all complete and board-verified**, see §4)
> Basis: [hal_v2 capability gap report](../../hal_v2/platforms/hailo15/docs/) (official Feature List 1.4 + Media Library / Imaging / DSP / HailoRT 1.12.0/5.3.0 + SDK sysroot headers + hailo-ai GitHub 1.12.x)
> Scope: from 51 identified gaps, select the subset that **belongs in the HAL layer**; provide API drafts, low-level mappings, and a batched delivery order.

---

## 1. Selection criteria

**Included** when all of the following hold:
1. It is a Hailo-15 hardware / official software stack **platform capability**, so a cross-platform abstraction makes sense (stub can simulate it);
2. Upper layers (runtime / Web / applications) have a direct consumption scenario;
3. A clear low-level API exists to map onto (medialib / v4l2 / HailoRT / libhailodsp), no reverse engineering or in-house algorithm needed.

**Excluded** (see §6): application-layer capabilities (webserver/RTSP/gRPC), system-level capabilities (secure boot/TRNG), hardware blocks with no product requirement (DSI display), and deep IQ tuning in the official Tuning-tool domain (Gamma/DCI/CAC/DPCC — already reachable through the existing `set_config_field` JSON channel).

## 2. Compatibility ground rules (binding for all batches)

Existing mechanisms (verified) dictate the extension style:

| Mechanism | Current state | How this plan uses it |
|---|---|---|
| ops function-pointer tables | `HalXxxOps`, resolved at link time; callers already have the `if (!HAL_CODEC_OPS.xxx)` NULL-check convention (`init_from_context` is the NULL precedent) | **new function pointers are always appended at the table tail**; platforms that do not implement them leave them NULL |
| config structs | callers zero-initialize with `{}` (e.g. `HalMediaImageConfig g_image_cfg{}`); `priv` opaque pointer last | new capabilities **prefer a dedicated struct + dedicated op**; when an old struct really must grow, only tail fields are appended and **0 = behavior unchanged** |
| error codes | `HAL_ERR_NOT_SUPPORTED` / `NOT_IMPLEMENTED` / `INVALID_STATE` already exist | reuse them; no new error codes |
| versions | `HAL_VERSION 0.1.0`, libaipc_hal `SOVERSION 2` | append-only changes → MINOR +1, SOVERSION unchanged; each module's `get_version` string bumps its minor |
| stub platform | `platforms/stub/` parallel to hailo15 | every new op ships a runnable stub (static simulation) so upper layers can integrate cross-platform |
| behavioral compatibility | — | new features default to off; when the new APIs are not called, behavior is byte-for-byte identical to the old version |

**Acceptance red line**: every pre-existing example (auto_af_test / test_media_all_func / ai_example_v2, etc.) must **compile, run and behave identically without a single line of modification** after the new headers land.

## 3. Batch plan

### Batch A — Encoding enhancements (hal_codec / hal_media) | milestone M1

Best cost/benefit: official SmartStream+ is the core H15 security selling point and everything underneath is already there.

| # | Capability | Size |
|---|---|---|
| A1 | ROI / smart encoding | M |
| A2 | Force keyframe | S |
| A3 | Encoder stream statistics query | M |
| A4 | SEI user metadata (phase 2, research first) | M |

**A1 ROI / smart encoding** — dedicated new struct, **`HalCodecConfig` untouched**:

```c
/* appended to hal_codec.h */
#define HAL_CODEC_ROI_MAX 10

typedef struct {
    float x, y, w, h;        /* normalized [0..1], union of ROIs = high-quality region */
} HalCodecRoi;

typedef struct {
    bool     enabled;              /* smart encoder master switch */
    int      background_qp_delta;  /* [0..16] QP added outside ROIs; 0 = default */
    uint32_t roi_count;            /* 0..HAL_CODEC_ROI_MAX when enabled */
    HalCodecRoi rois[HAL_CODEC_ROI_MAX];
} HalCodecRoiConfig;

/* appended at the HalCodecOps tail */
int (*set_roi_config)(void *codec_ctx, const HalCodecRoiConfig *config);
int (*get_roi_config)(void *codec_ctx, HalCodecRoiConfig *config);
/* phase 2: AI-detection linkage (person/vehicle/face/license_plate -> dynamic ROI) */
int (*set_analytics_labels)(void *codec_ctx, const char *const *labels, uint32_t count);
```

- Low-level mapping: medialib `smart_encoder_config_t` (smart_encoder.enabled / rois / background_qp_delta / analytics_labels).
- **Constraint passthrough**: officially H.264 + CVBR only → return `HAL_ERR_NOT_SUPPORTED` for other codec/RC combos, with the constraint stated in the error message.
- Phase 2 `set_analytics_labels` shares the detection-result pipeline with the DPM `attach_frame_analytics`.
- stub: store the ROI list, `get_roi_config` reads it back.

**A2 Force keyframe**:

```c
int (*force_idr)(void *codec_ctx);     /* medialib encoder force-keyframe API */
```

**A3 Encoder stream statistics**:

```c
typedef struct {
    float    fps;               /* -1.0 = unknown */
    uint32_t bitrate_kbps;      /* 0 = unknown */
    int      qp_avg;            /* -1 = unknown */
    uint64_t frames_encoded;    /* 0 = unknown */
} HalCodecStreamStats;

int (*get_stream_stats)(void *codec_ctx, HalCodecStreamStats *out);
```

- Low-level mapping: medialib `get_current_fps()` + `encoder_monitors` (bitrate_monitor / cycle_monitor).
- FROM_MEDIA-style codec_ctx must locate the encoder by stream_id; HW style queries directly.

**A4 SEI user metadata** (research before implementing): the official `user_metadata_sei` is **UUID+JSON configuration-style** (not a per-frame injection API); confirm whether medialib supports updating the payload at runtime. If config-only, shape it as `set_sei_metadata(codec_ctx, uuid, json)` whole-segment updates, to distribute AI metadata alongside the stream. **Nothing enters the header until the shape is decided.**

### Batch B — ISP fine control (hal_isp) | milestone M2

All appended at the `HalIspOps` tail; the video_ctx-first-parameter convention is kept.

| # | Capability | Size |
|---|---|---|
| B1 | AWB manual gains + CCM | M |
| B2 | Independent 3DNR control | S |
| B3 | Defog (research before deciding) | M |
| B4 | AE statistics readback | M |
| B5 | HDR exposure ratio runtime adjustment | S |
| B6 | Digital zoom limit widened to 31x | S |

**B1 AWB manual white balance**:

```c
typedef struct {
    bool  enabled;                          /* true = manual WB; false = auto (fields ignored) */
    float r_gain, gr_gain, gb_gain, b_gain; /* 1.0 = neutral */
    float ccm[9];                           /* optional 3x3 color matrix; zeros = identity */
} HalIspWbConfig;

int (*set_wb_config)(void *video_ctx, const HalIspWbConfig *config);
int (*get_current_wb_config)(void *video_ctx, HalIspWbConfig *config);
```

- Mapping: v4l2 `WB_R_GAIN / WB_GR_GAIN / WB_GB_GAIN / WB_B_GAIN` + `wb_cc_matrix` + `AWB_MODE` switched to manual (setting `enabled=false` restores auto).

**B2 3DNR**:

```c
typedef struct {
    bool enabled;
    int  strength;          /* [0..100] */
} HalIspNr3dConfig;

int (*set_3dnr_config)(void *video_ctx, const HalIspNr3dConfig *config);
```

- Mapping: iq_settings JSON (`3dnr.enable` + `A3dnrv1` strength), pushed through the existing `set_config_field("frontend.iq_settings...")` channel to trigger a 3A hot reload (daemon polls at 1 s; no pipeline restart needed).

**B3 Defog**: the feature list §2.2 names it an official capability, but **no runtime switch was found** in the medialib 1.12 headers or JSON templates. Decide between:
- (a) the Imaging layer exposes a v4l2/iq channel → build `set_defog_config` following the B2 pattern;
- (b) it only exists in the Tuning-tool domain → no HAL C API; document the JSON path under `docs/references` and mark the gap report "not opened by the official software stack".
**Do a 30-minute SDK/doc probe first; no pre-set conclusion.**

**B4 AE statistics** (mirroring the existing AF statistics trio model):

```c
typedef struct {
    uint32_t hist[256];        /* luma histogram */
    uint32_t luma[25];         /* 5x5 mean-luma grid */
    uint64_t frame_id;         /* 0 = unknown */
    uint64_t timestamp_ns;
} HalIspAeStats;

int (*subscribe_ae_stats)(void *video_ctx);
int (*wait_ae_stats)(void *video_ctx, int timeout_ms);
int (*get_ae_stats)(void *video_ctx, HalIspAeStats *out);
```

- Mapping: v4l2 `isp_ae_hist` (256 bins) + `isp_ae_luma` (5x5); events via `HAILO15_UEVENT_ISP_STAT`/EXP_STAT (same mechanism as AF).

**B5 HDR exposure ratios**:

```c
int (*set_hdr_ratios)(void *video_ctx, float ls_ratio, float vs_ratio);
```

- Mapping: medialib `Frontend::set_hdr_ratios`; return `HAL_ERR_INVALID_STATE` for non-HDR profiles.

**B6 Zoom limit**: widen the `digital_zoom_value` documentation from [1..5] to [1..31] (medialib `DIGITAL_ZOOM_MODE_MAGNIFICATION` hardware cap is 31x); the implementation already passes it through as a float — only comments and validation change.

### Batch C — Events, observability and snapshots (hal_media / hal_video / hal_inference) | M1+C2/C3, M2+C1/C4

| # | Capability | Module | Size |
|---|---|---|---|
| C1 | Motion detection | hal_media | M |
| C2 | Thermal throttling event subscription | hal_media | S |
| C3 | SoC temperature | hal_inference (extend existing system stats) | S |
| C4 | Multi-stage snapshots | hal_video | M |

**C1 Motion detection** (official frontend domain, hence media rather than isp):

```c
typedef struct {
    bool  enabled;
    int32_t roi_x, roi_y, roi_w, roi_h;   /* pixels; all-zero = full frame */
    int   sensitivity;                     /* 0..4 -> LOWEST..HIGHEST */
    float threshold;                       /* 0..1 changed-pixel ratio to trigger */
} HalMotionConfig;

typedef void (*HalMotionCb)(void *user, bool motion_detected,
                            uint64_t frame_id, uint64_t timestamp_ns);

/* appended at the HalMediaOps tail */
int (*set_motion_config)(void *media_ctx, const HalMotionConfig *config);
int (*subscribe_motion)(void *media_ctx, HalMotionCb cb, void *user);
int (*unsubscribe_motion)(void *media_ctx);
```

- Mapping: medialib `motion_detection_config_t` + the output buffer's `motion_detected` metadata; callback model aligned with the existing alarm-subscribe style. The motion bitmask (GRAY8) is not exposed in this batch; deferred to v2.

**C2 Thermal throttling events** (upgrade after-the-fact error codes into proactive events):

```c
typedef void (*HalThrottlingCb)(void *user, bool restricted, const char *active_profile);

int (*subscribe_throttling)(void *media_ctx, HalThrottlingCb cb, void *user);
int (*unsubscribe_throttling)(void *media_ctx);
int (*get_throttling_state)(void *media_ctx, bool *restricted);
```

- Mapping: medialib `subscribe_to_throttling_state_change` / `get_throttling_state`.

**C3 SoC temperature** — extend the existing struct (appended fields, 0/negative = unknown):

```c
/* appended at the tail of the system perf-stats struct */
float soc_temp_c;        /* NNC on-die temperature (ts0), -1.0 = unknown */
float soc_temp_c1;       /* second sensor (ts1), -1.0 = unknown */
```

- Mapping: HailoRT `get_chip_temperature()`; returned by the same query as the existing npu/cpu/ram/dsp utilization; the `query_system_performance_stats` signature is unchanged.
- Zero-initializing callers get -1.0 (old callers do not read the new fields; behavior unchanged).

**C4 Multi-stage snapshots** (forensics + diagnostics):

```c
typedef enum {
    HAL_SNAPSHOT_POST_ISP = 0,   /* NV12, ISP-processed */
    HAL_SNAPSHOT_PRE_ISP_RAW,    /* raw16, pre-ISP (diagnostics; 1.12.0 capability) */
    HAL_SNAPSHOT_PER_STREAM,     /* one multi-resize output stream */
    HAL_SNAPSHOT_ENCODER,        /* encoder-side frame */
} HalSnapshotStage;

typedef struct {
    HalSnapshotStage stage;
    char  stream_id[16];         /* PER_STREAM / ENCODER */
    char  out_path[256];         /* written file path (impl fills) */
} HalSnapshotRequest;

typedef struct {
    char  path[256];
    uint32_t width, height;
    uint32_t bytes_written;
} HalSnapshotResult;

/* appended at the HalVideoOps tail */
int (*request_snapshot)(void *video_ctx, const HalSnapshotRequest *req,
                        HalSnapshotResult *out, int timeout_ms);
int (*list_snapshot_stages)(void *video_ctx, char (*stages)[32], int max_stages);
```

- Mapping: medialib `enable_snapshot(true)` + `SnapshotManager::request_snapshot(frames=1, stages)` / `list_available_stages`; JPEG-encoded snapshots go through the existing MJPEG codec path (combined example provided).
- PRE_ISP_RAW needs `MEDIALIB_SNAPSHOT_ENABLE`; the implementation probes at init and returns `HAL_ERR_NOT_SUPPORTED` when unavailable.

### Batch D — Inference / GenAI enhancements (hal_inference / hal_genai) | milestone M3

| # | Capability | Size |
|---|---|---|
| D1 | On-chip NMS runtime tuning | S |
| D2 | GenAI context (KV cache) persistence | M |
| D3 | ASR speech2text (research item) | L |

**D1 NMS runtime tuning**:

```c
typedef struct {
    float    score_threshold;       /* <=0 = keep current */
    float    iou_threshold;         /* <=0 = keep current */
    uint32_t max_proposals_per_class; /* 0 = keep current */
    uint32_t class_filter_mask[8];  /* 256-class bitmask; all-zero = no filter */
} HalNmsParams;

/* appended at the HalInferenceOps tail */
int (*set_nms_params)(void *session, const HalNmsParams *params);
```

- Mapping: HailoRT `set_nms_score_threshold / set_nms_iou_threshold / set_nms_max_proposals_per_class / set_nms_classes_filter_mask`.
- Valid only for `is_nms` models; `HAL_ERR_INVALID_STATE` otherwise.

**D2 GenAI context persistence**:

```c
/* appended at the HalGenaiOps tail */
int (*save_context)(void *session, void **buf, size_t *len);   /* HAL allocates */
int (*load_context)(void *session, const void *buf, size_t len);
int (*get_context_usage)(void *session, size_t *used, size_t *capacity);
int (*free_context_buffer)(void *buf);
```

- Mapping: HailoRT GenAI `save_context / load_context / get_context_usage_size / max_context_capacity`.
- Use cases: VLM multi-turn context save/restore; resumable conversations.

**D3 ASR**: the SDK has `genai/speech2text/` headers, but HailoRT docs assign GenAI to H10H while the H15 1.12.0 release notes already mention a VLM preview. **Verify on-board that a speech2text HEF runs before creating a work item**; if feasible, add `create(SPEECH2TEXT)/write_pcm/read_text` in the same style as hal_genai.

### Batch E — Additional DSP operators (hal_dsp) | milestone M3

All appended at the `HalDspOps` tail as thin wrappers, reusing the existing `HalDspImage`/job model.

```c
/* arbitrary-angle rotation (existing flip_rotate only does 0/90/180/270) */
int (*rotate)(void *dsp_ctx, const HalDspImage *src, HalDspImage *dst, float theta_deg_cw);

/* mesh dewarp (float grid at the HAL layer; impl converts to Q15.16 + 64-cell) */
typedef struct {
    uint32_t grid_cols, grid_rows;   /* mesh grid points per axis */
    float   *xy;                     /* grid_cols*grid_rows*2, dst-grid -> src coords */
} HalDspMesh;
int (*dewarp)(void *dsp_ctx, const HalDspImage *src, HalDspImage *dst, const HalDspMesh *mesh);

/* image enhancement (blur/bilateral/sharpness/contrast, one combined operator) */
typedef struct {
    int   blur_level;        /* 0..4, 0 = off */
    bool  bilateral_enable; float bilateral_sigma;   /* 0..255 */
    int   sharpness_level;   /* 0..3, 0 = off */ float sharpness_amount; int sharpness_threshold;
    float contrast;          /* 1.0 = neutral */
    int   brightness;        /* -128..127, 0 = neutral */
} HalDspEnhanceParams;
int (*enhance)(void *dsp_ctx, const HalDspImage *src, HalDspImage *dst,
               const HalDspEnhanceParams *params);

/* telescopic progressive scaling (anti-aliasing for large-ratio downscales) */
int (*multi_crop_resize_telescopic)(void *dsp_ctx, const HalDspImage *src,
                                    const HalDspCropResizeParam *params, uint32_t count);
```

- Mapping: `dsp_rotate / dsp_dewarp / dsp_image_enhancement (via dsp_frontend_process or standalone) / dsp_telescopic_multi_crop_and_resize`.
- Sync/async: synchronous versions first, consistent with existing operators; async jobs extend the existing `submit/wait` model later.
- Priority: rotate (45° PTZ OSD scenario) > dewarp (custom correction) > enhance > telescopic. Gaussian/Laplacian filters are **not done** (no upper-layer consumer; add when needed).

## 4. Milestones and dependencies

```
M1 (low-risk pure additions, merged first) — DONE (2026-09-01, both platforms compile-verified)
 |- A1 A2 A3   smart encoding + stats (set_roi_config / get_roi_config / force_idr / get_stream_stats)
 |- B6         zoom 31x (doc widening + dynamic_change_image_config entry validation [1..31])
 `- C2 C3      thermal throttling events (subscribe_throttling / get_throttling_state)
               + SoC temperature (soc_temp_c/c1)
   Note: landing A3 corrected two points of the plan — frames_encoded was dropped
   (the underlying monitors have no cumulative frame count, only a 60-frame window
   counter; avoids a forever-0 fake field); the bitrate unit was confirmed to be
   bytes/s (from medialib hailo_encoder_impl.cpp), converted to kbps at the HAL.

M2 (ISP fine control + events/snapshots) — DONE (2026-09-01, both platforms compile-verified)
 |- B1 B2 B4 B5   manual AWB / 3DNR / AE stats / HDR ratio
 |- B3            defog: decided (b), no C API — SDK 1.12 headers/JSON have no
 |                runtime channel (the feature list names the capability but the
 |                official software stack does not open it; noted in the gap report)
 `- C1 C4         motion detection / multi-stage snapshots
   Implementation adjustments:
   - B4 ships get_ae_stats polling only, no subscribe/wait trio (the existing AF
     subscribe/wait was already a NOT_SUPPORTED placeholder and polling is the
     real usage; two fewer dead ops)
   - B1 CCM: the medialib v4l2 mapping has no wb_cc_matrix control; a non-zero
     CCM returns NOT_SUPPORTED (field kept in the struct; implement when the
     channel opens)
   - B2 3DNR: control-name probing (candidate list incl. isp_3dnr_strength),
     converged after board verification
   - C4 snapshots are async arm-and-wait (the official SnapshotManager is async):
     request_snapshot(stage) arms it, files land under /tmp/medialib_snapshots/<ts>/;
     stage is a string, not an enum (stages are registered dynamically by the pipeline)
   - motion events fire on state transitions in the frontend bridge (not per-frame)

M3 (inference / DSP) — DONE (2026-09-01, both platforms compile-verified)
 |- D1 D2         NMS parameters / KV cache
 `- E rotate -> dewarp -> telescopic
   Implementation adjustments:
   - D1 moved from a "runtime op" to create-time config HalInferenceConfig.nms
     (HailoRT InferModel set_nms_* take effect at configure(); changing NMS on a
     live session requires rebuilding the network group — a runtime op would be
     dishonest)
   - D2 save_context returns a HAL-allocated heap buffer, freed by
     free_context_buffer; load requires the same model
   - E-dewarp: float grid at the HAL layer, impl converts to Q15.16 (NV12 +
     bilinear hardware constraints passed through)
   - E-enhance dropped: the SDK has no standalone image-enhancement operator
     (only a deprecated combined API and the dsp_frontend_process composite
     entry); wrap the composite later when a consumer appears

Dropped at review (2026-09-01):
 |- F object tracking -> moved to the application layer (pure software, no
 |   hardware coupling; apps link libhailopp.so directly, see §6)
 `- G SoC OTA        -> already done at the application layer; HAL exposure
     would be duplication

Board verification (2026-09-01, 192.168.93.58 / IMX678 4K) — M1/M2/M3 all passed:
  M1: enc_roi write/read-back consistent (bg_qp+ROI) / force_idr / enc_stats
      (fps + bitrate window) / media_throttling (FULL_PERFORMANCE) / zoom 20x
      (bitrate drops with cropping 14322 -> 5691 kbps)
  M2: isp_wb lock/restore (manual state stable across queries) / 3dnr write /
      ae_stats (256-bin histogram, 921600 samples + luma grid) / hdr_ratios
      (both ls=20/8 effective under an HDR profile; non-HDR correctly rejected) /
      motion config round-trip + subscription / snapshot multi-stage real files
      on disk (post_isp 4K, three multiresize stages, two encoder stages)
  M3: dsp dewarp identity-mesh numerically PASS (max_delta=0) / rotate 45°
      visually geometrically correct / telescopic wrapper correct but see the
      userptr limitation below
  Also: JPEG/MJPEG chain passed end-to-end (hal-jpeg-web-test: single frame
      33 KB valid JFIF, exact 10.00 fps, multipart stream + on-demand snap);
      fixed patch_json missing the JPEG branch, which made
      add_streams_batch(mjpeg) silently degrade to H264.

Platform limitations found on board (recorded as-is):
  1. WB manual gains: isp_wb_*_gain are AWB-engine state outputs (writes are
     accepted but have no imaging effect; R gain 3.9 vs 1.0 leaves the output
     R/B ratio unchanged); set_wb_config manual mode was kept as "lock current
     WB" (aw_drv4 off, verified effective); non-unit gains returned
     NOT_SUPPORTED  [superseded by round 4 below]
  2. DSP telescopic / multi-plane operators: with userptr, when the Y/UV
     physical pages straddle the 16 MB idma window the firmware errors out
     (non-deterministic); production paths use DMABUF (naturally satisfied
     inside the media pipeline)
  3. snapshot stage registration happens at component construction: enable
     was moved to media init (enabling later yields an empty stage set)
  4. motion events trigger on scene jumps: API/subscription paths verified;
     the event itself needs a real motion source on site
  5. the test REPL (read_line_raw) needs a TTY: automation drives it over a
     pty (the on-board /tmp/drive_hal_cli.py pattern)

Second self-driven verification round (late 2026-09-01, everything not needing
human intervention done):
  + D1 NMS create-time parameters: verified effective — yolov8n (official model
    zoo v5.3.0 HEF with on-chip NMS) thr=0.001 -> 8 boxes, thr=0.6 -> 1 box
    (hal-nms-threshold-test)
  + B2 3DNR effect: driver-level measurement strength=128 cuts flat-area
    high-frequency noise energy by 40% (38.58 -> 23.13, mean luma unchanged to
    rule out exposure shifts); impl corrected (write isp_3dnr_enable first,
    then strength; 0..100 -> 0..128 mapping)
  + Compatibility regression (on the new .so): udp_stream_test /
    video_test_sub-v2 (3 encode streams, pkts=120 idr=4) / dsp-media-udp-resize
    (748 frames sustained) / test_media_all_func / inference-perf-sample /
    audio (ALSA enumeration normal) / MCU+factory (ttyS1/2 communication
    normal) / AF statistics path (API works; sum=0 was pre-existing hardware
    state)
  - V4 GenAI context: the public model zoo (hailo_model_zoo_genai) only has
    hef_h10h keys, no H15 LLM/VLM HEFs — needs the Developer Zone/FAE channel;
    could not be self-verified
  - V1 motion events: root-caused — under medialib's config_attacher mechanism
    motion detection reads a "config snapshot attached to the input buffer";
    set_override_parameters only edits the in-memory profile and never
    persists to the on-disk JSON; even a full reinit rebuilds from the disk
    JSON -> the module never runs (proven by motion_detection_buffer always
    empty). Fixing needs JSON-level persistence of the motion section +
    reinit; filed as a dedicated work item
  Third round (2026-09-02, (1) real preprocess + (2) NMS decoding):
  + tensor_from_frame_ex (appended at the ops tail; the old tensor_from_frame
    stays a bare copy and the header now says so): letterbox + NV12->RGB
    (BT.601) + resize/quantization inside the HAL (HailoRT
    InputTransformContext, cached per session) + normalize (uint8->f32);
    zero-copy fast path on exact match. Two traps defused: the transform
    expects image order (RGB888) while the model input stream reports tensor
    order (NHWC) — conversion needed; NV12 cannot be expressed by its shape
    model, so NV12 sources get a CPU RGB conversion first (production NV12
    models take the fast path, unaffected)
  + hal_inference_decode_nms (pure-logic function in common): decodes the
    on-board-measured layout [count:float32][n x {ymin,xmin,ymax,xmax,score}];
    the layout is documented in hal_inference.h
  + End-to-end board verification: yolov8n real frames through _ex full-HAL
    preprocessing -> 10 boxes (thr=0.001) vs 1 box (thr=0.6), best score 0.751
    with normalized coordinates
  Fourth round (2026-09-02, deep fixes driven by official-doc research):
  + B1 WB manual gains fully fixed and visually verified: root cause = the
    mandatory official precondition was missing before writing gains (imaging
    guide line 3492 "wb control must disable awb") + the algorithm block to
    freeze is awbv2, not aw_drv4 (the latter is Auto-WDR). New sequence:
    awbv2 freeze -> isp_awb_enable=0 -> isp_awb_mode=0 -> four Q8.8 gains
    (+ optional isp_wb_cc_matrix). Visual measurement: at R=3.9x the picture
    R/B goes 0.96 -> 4.24
  + V1 motion events fully working: HAL in-house frame-difference engine
    (16x16 block-mean diff on the smallest output stream,
    sensitivity->pixel-delta threshold mapping, state-transition trigger).
    On board, zoom toggles fired 12 START/STOP events PASS. Final word on the
    official-stack cause of death: every official config has motion disabled
    and resolution.stream_id never filled; force-filling it makes medialib
    treat it as a 4th output stream -> perform_multi_resize errors every
    frame, the frontend stalls
  + B4 AE luma grid fixed: isp_ae_luma is a u8x25 control (different element
    type than the u32 histogram); reading it as bytes gives luma_sum=541
    non-zero
  + A1 ROI effect measured: bg_qp=15 + a small ROI drops bitrate
    2801 -> 463 kbps (-83.5%)
  + A2 force_idr bitstream-level measurement: Annex-B scan of subscribed
    packets, IDR slices 3 -> 5 (+2)
  + B5 HDR ratio config-level confirmation: active profile JSON lsRatio
    16.0 -> 24.0
  + tensor_from_frame_ex path matrix all PASS: NV12 640x384 (10v1) / RGB24
    (8v1) / BGR24 (6v1) / NV12 320x240 scaled+letterboxed (7v1); the resize
    implementation switched to CPU bilinear because InputTransformContext does
    not support arbitrary rescale (noted in code comments)
  - B2 3DNR: strength sweep verdict — merely enabling 3DNR (even
    strength=0) doubles noise energy 16 -> 33, with no gradient between
    strength 0/25/50/100. Conclusion: isp_3dnr_strength is not an independent
    denoise strength; it must act together with isp_3dnr_auto_level /
    delta_factor / motion_factor and the A3dnrv1 auto module; writing strength
    alone is meaningless. This is 3A-tuning-framework territory — a simple
    HAL scalar set cannot drive it correctly. Interface kept, documented as
    "requires 3A configuration". (The earlier -40% from a direct v4l2-ctl
    write was an artifact of enable not being written with only strength
    hitting; clarified together with the scalar fix)
  * D2/D3 major correction: H10H HEFs can be used directly on H15 (parse-hef
    shows "HEF Compatible for: HAILO15H, HAILO10H"). Measured: Whisper-Tiny
    (78 MB) runs on H15 in hw_only mode (33 FPS, encoder+decoder dual network
    groups); Qwen2-1.5B (266 MB) fails to load due to the H15 VDMA
    scatter-gather page-table cap ("failed to set sg list for user buffer",
    512-entry SG table) — large models are limited by board-level DMA
    resources, not a HAL defect. The GenAI HAL uses shared server mode; the
    server crashed ("Failed with result 'signal'") when loading GenAI, root
    cause being an indirect manifestation of the same VDMA limit. Conclusion:
    small models (Whisper / <= ~100 MB) run on H15; large LLMs need more
    board memory/DMA configuration or a DFC build trimmed for H15.
  + AF statistics: API fully consistent with the reference example
    (auto_af_test); measured non-zero focus energy (sum/luma vary frame to
    frame); the AF chain works
  + DSP telescopic DMABUF: measured — the DMABUF fd path passes 5/5 stably
    while the MALLOC/CMA path fails ret=-2801 — confirming DMABUF is the
    correct usage for telescopic (matches the official "use DMABUF in
    production"). userptr/CMA randomly fails across the 16 MB idma window.
  + AF statistics: verified working (after manually setting a window,
    sum=[28682868,...] non-zero focus energy). Note: the Hailo driver requires
    a fixed set of 3 AF windows; when 1 window is set the driver fills default
    320x180 windows (win2/win3), so sum[1]/sum[2] are default-window data to
    be ignored; only sum[0]/luma[0] correspond to the user window. The earlier
    "sum=0 needs a lens module" conclusion was a pty-driver timing artifact
    and is withdrawn.
  + AE statistics: root cause same family as AF — missing isp_ae_enable.
    With enable + window added, luma_sum=693 non-zero (the earlier 541 was a
    misread; now stably non-zero with the enable in place).
  + B2 3DNR strength sweep: enabling 3DNR doubles noise 16 -> 33, no gradient
    across strengths; final verdict: needs A3dnrv1 3A-framework coupling, not
    an independent scalar (interface kept, documented).
  By-product of compatibility testing — fixed a pre-existing bug: the HW
    encoder JSON template lacked smart_encoder.analytics_labels (mandatory in
    the SDK schema), making the video_test_sub_v2 encoder init fail instantly
    (present at HEAD, not introduced by this change); also aligned
    background_qp_delta with the actual schema range [1..15]

Research items (no commitment): A4 SEI shape, D3 ASR feasibility
```

Each batch delivers = headers + hailo15 implementation + stub implementation + examples (extending the existing test_media_all_func / test_peripheral_all_func subcommand style) + `docs/architecture/hal_v2_overview.md` interface-table sync + `get_version` bump.

## 5. Testing and acceptance

1. **Compatibility regression (red line)**: pre-existing examples compile and run with zero modification; size changes of old structs like `HalCodecConfig` / `HalMediaImageConfig` are tail-appends only; `{}` zero-initialization semantics = new feature off.
2. **Functional acceptance**: every new op has at least one example path (real hailo15 board + stub both); constrained capabilities (ROI encoding limited to H264+CVBR, HDR ratio limited to HDR profiles, NMS limited to is_nms models) have negative cases returning the correct error code.
3. **Event-style items** (C1/C2/B4): verify subscribe/unsubscribe/timeout paths, behaviorally consistent with the existing AF subscription model.
4. Rough sizing: S < 1 day / M 1–3 days / L 3+ days (including integration, per existing code style).

## 6. Explicitly not done (this cycle) and why

| Item | Reason |
|---|---|
| **Object tracking (former batch F)** | Pure software algorithm, no hardware coupling: HailoPP is a standalone user-space CPU library (`libhailopp.so` + `hailopp/hailotracker.h`) that the application layer can link directly; tracking input is already-decoded platform-neutral detection boxes (unlike hal_postprocess which must understand the HailoRT NMS layout); algorithm and parameter choice is a product decision, and swapping it at the application layer never touches the HAL ABI (dropped at the 2026-09-01 review) |
| **SoC-side system OTA (former batch G)** | The application layer already has the swupdate upgrade chain; exposing it again in the HAL would duplicate; the existing `hal_ota.h` continues to handle only MCU firmware (Ymodem) (dropped at the 2026-09-01 review) |
| DSI display output (feature list §2.6) | No display product requirement; separate batch if needed |
| gRPC cross-process medialib service | Architectural change; official status is also preview |
| RTSP service / webserver / ZMQ / WS metadata channels | Application-layer responsibility; the HAL already has UDP+RTP video channels |
| Dual-sensor pipeline | Current hardware is single-sensor; even the official dual-sensor path has limitations such as no HDR |
| secure boot / TRNG / crypto / watchdog | System level, ATF/U-Boot domain |
| Gamma/DCI/CAC/DPCC/LSC deep-IQ C APIs | Official Tuning-tool domain; already reachable via the `set_config_field` JSON channel — documentation suffices (LSC was a batch-B documentation item) |
| Gaussian/Laplacian DSP filters, multi-blend details | No upper-layer consumer |
| UVC input / SoC PWM / generic UART/SPI/I2C passthrough | No approved requirement; the existing NOT_SUPPORTED semantics for PWM are already explicit |
| On-chip AnalyticsDB | Overlaps with the runtime-side storage responsibility; waiting for the runtime layer to weigh in first |
