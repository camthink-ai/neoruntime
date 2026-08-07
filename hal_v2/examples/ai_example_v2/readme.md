Example pipeline overview:
Initialize MediaLibrary -> select profile -> disable auto feed -> pick first stream + encoder for preview -> subscribe to stream + codec callbacks -> enqueue frames in stream callback (if multiple models share frames, also enqueue to inference queue)
                                                                                                    -> codec callback pushes packets to UDP (if configured)
Initialize model(s) -> load model(s) -> query model info -> pick a stream closest to model resolution -> subscribe to that stream output (if it is the same as the preview stream, reuse the same callback)

Initialize preview thread -> wait for preview frames -> wait up to 10ms for AI results -> draw overlay -> feed into encoder

Initialize inference thread -> wait for AI frames -> resize if needed -> run inference -> if a second-stage model exists, crop ROIs of interest (e.g., faces) on the pre-resize frame -> resize to stage-2 input size -> run stage-2 inference -> map results back to the original frame coordinates -> publish results
                                                                   -> single model: publish results

After building, the executable is `hal-ai-example-v2`. Run `./hal-ai-example-v2 --help` for all flags.

## CLIP example

```bash
./hal-ai-example-v2 --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --model /home/root/apps/clip/resources/clip_vit_b_32_image_encoder_nv12.hef \
  --post-type embedding \
  --post-file ./clip_vit_b_32_embedding_post.json \
  --clip-text-default \
  --post-json '{"prompts":["person","phone","cat","bottle"],"negative_prompts":[],"score_threshold":0.0,"top_k":5,"match_policy":"softmax"}' \
  --udp 192.0.2.93:5555
```

## Face landmarks example

```bash
./hal-ai-example-v2 --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json \
  --model /home/root/apps/face_landmarks/resources/face_landmarks_lite.hef \
  --udp 192.0.2.93:5555
```

## Detection example

```bash
./hal-ai-example-v2 --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json \
  --model /home/root/apps/face_landmarks/resources/hailo_yolov8s_384_640.hef \
  --post-type detection \
  --post-file /home/root/apps/webserver/resources/configs/yolov5.json \
  --post-json '{"backend_function":"hailo_yolov8s","label_offset":0}' \
  --udp 192.0.2.93:5555
```

## Classification (ImageNet-style, single-input `HxWx3` RGB/BGR)

On Hailo-15, `HAL_INFERENCE_OPS.tensor_from_frame()` does not perform NV12→RGB conversion. For single-input RGB/BGR classifiers, this example resizes to model size in NV12, then uses DSP `convert_format` to NV12→RGB or NV12→BGR before inference.

If no usable vendor classification postprocess is available (common on small rootfs), the example decodes the first output tensor on the host (optional softmax + top-k). Use Torchvision-style `imagenet_class_index.json` for human-readable labels in logs and on-screen OSD.

```bash
./hal-ai-example-v2 --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --profile 0 \
  --model ./vit_large.hef \
  --post-type classification \
  --preprocess-color nv12_to_rgb \
  --classification-labels-json ./imagenet_class_index.json \
  --post-json '{"output_activation":"softmax","top_k":5}' \
  --udp 192.0.2.93:5555 --verbose
```

- Use `--preprocess-color nv12_to_bgr` if the model was trained for BGR order.
- For NV12 two-plane inputs (Y + UV), the example feeds NV12 directly; `--preprocess-color` does not apply on that path.
- Optional `--post-file` / `--post-json` can still point at a vendor `backend_lib_path` / `backend_function` when you have a matching `.so` on the device (see `data/classification_wrapper.example.json` as a template).

### Deploy note (OSD text on the encoded stream)

Classification labels on the video overlay are drawn by HAL (`hal_draw_cpu` / `hal_draw_hailo15`). After changing draw behavior, deploy the rebuilt **`libaipc_hal.so`** (or your image’s HAL library name) together with **`hal-ai-example-v2`**, not only the executable.

## Segmentation / DPM (Dynamic Privacy Mask, LinkNet)

`--post-type segmentation` is supported. For **dynamic privacy mask**, enable **`--dpm`**: stage 1 runs a **detector** on the full frame; stage 2 **crops each ROI** and runs a **LinkNet-style segmentation** HEF, then aggregates masks (mosaic / blur / overlay).

Example HEF (NV12 in, two `256×256×1` outputs): `linknet_mbv1_ss_dpm_256.hef` under the dynamic privacy mask app. On Hailo-15, segmentation postprocess defaults to **`/usr/lib/hailo-post-processes/liblinknet_post.so`** with **`linknet_post`**; you only need `--post-file` / `--post-json` if your image uses a non-default config.

```bash
./hal-ai-example-v2 \
  --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --profile 0 \
  --dpm \
  --model /home/root/apps/dynamic_privacy_mask/resources/linknet_mbv1_ss_dpm_256.hef \
  --detector-model /home/root/apps/face_landmarks/resources/hailo_yolov8n_384_640.hef \
  --det-post-file /home/root/apps/webserver/resources/configs/yolov5_personface.json \
  --udp 192.0.2.93:5555 --verbose
```

- **`--dpm-labels`** (default `person,vehicle`): comma-separated allowlist of **detector** `label` strings; only matching boxes get segmented. Example: `--dpm-labels person`.
- **`--dpm-mode`**: `mosaic` (default), `blur`, or `overlay`.
- Other knobs: `--dpm-max-rois`, `--dpm-mask-size`, `--dpm-smooth-alpha`, `--dpm-block-size` (see `--help`).
- Do **not** use **`--preprocess-color nv12_to_rgb`** for this LinkNet: input is **NV12** (two-plane), same path as other NV12 models in this example.

### Render paths

The segmentation mask can be rendered onto the encoded stream two ways:

- **Default** (no extra flag): the app rasterizes the aggregated mask into a 4×4-quantized DSP bitmask and applies it in-place via `HAL_DSP_OPS.privacy_mask` before encode. Shape follows the mask; does not use the media library blender.
- **`--dpm-attach`**: feeds each per-ROI segmentation mask to the media library DSP privacy-mask blender (dynamic path) via `HAL_MEDIA_OPS.attach_frame_analytics`. The blender draws the silhouette during encode. This is the reference path for the HAL dynamic-privacy-mask API — see `hal_v2/doc/hal_dpm_usage_guide.md`.

```bash
./hal-ai-example-v2 \
  --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --profile 0 \
  --dpm --dpm-attach \
  --model /home/root/apps/dynamic_privacy_mask/resources/linknet_mbv1_ss_dpm_256.hef \
  --detector-model /home/root/apps/shared/resources/hailo_yolov8n_384_640.hef \
  --det-post-file ./yolov5_personface.json \
  --dpm-labels person \
  --udp 192.0.2.93:5555 --verbose
```

`--dpm-attach` notes:

- Enables the dynamic privacy-mask config (`dynamic_enabled` + `masked_labels` + `label_to_class_id`) on the active profile at startup.
- `--dpm-labels` must list the segmentation labels to mask; the first label is used as the per-frame attach label (single-class segmentors).
- The `linknet_mbv1_ss_dpm_256` model is **stretch-trained** (it fills the 256×256 input, ignoring letterbox). The attach path therefore crops with **STRETCH** and resamples the stretched mask back into the letterbox content region (by ROI aspect, padding zeroed) before feeding the blender — matching the blender's letterbox contract without fighting the model's training.
- `--dpm-attach-recttest`: diagnostic mode that fills the content region solid (ignores the model output). The blur should land exactly on the detection bbox — used to verify DSP placement. Not for production.

Standalone **`--post-type segmentation --model …linknet…`** (full frame, no `--dpm`) is possible if the pipeline resolution matches the model; this DPM HEF is intended for **detector ROI → segment** use.

## Monocular depth / SCDepthV3 (`--post-type depth`)

Hailo-15 decodes the logits tensor **in HAL** (same steps as `hal_v2/doc/hailo-apps/hailo_apps/postprocess/cpp/depth_estimation.cpp` and `hailo_apps/cpp/depth_estimation_mono/mono_depth_estimation.cpp`). No `lib*_postprocess.so` is required.

- Default output name: `scdepthv3/conv31`. Override with JSON `scdepth_output_name` or `output_tensor_name`.
- Quantized **uint8 / uint16** logits use Hailo tensor `qp_zp` / `qp_scale`. For **float32** vstreams (see `depth_estimation_mono`), set `"depth_float32": true`.
- On-screen **pseudo-color depth thumbnail** (top-right; per-frame min–max) is enabled for `--post-type depth` in this example so FPS stays high. Rebuild **`libaipc_hal.so`** + **`hal-ai-example-v2`** after changing draw code. Tune size via `HalDrawConfig.depth_thumbnail_max_width` / `depth_thumbnail_margin` (defaults set in `ai_example_v2.cpp` when depth is active).

Verified command (HEF + post JSON next to the binary; use `data/scdepth_depth_post.example.json` when running from the example tree):

```bash
./hal-ai-example-v2 \
  --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json \
  --profile 0 \
  --model ./scdepthv3.hef \
  --post-type depth \
  --post-file ./scdepth_depth_post.example.json \
  --preprocess-color nv12_to_rgb \
  --udp 192.0.2.93:5555 \
  --verbose
```

- Template in repo: `examples/ai_example_v2/data/scdepth_depth_post.example.json` — copy next to `hal-ai-example-v2` as `scdepth_depth_post.example.json` (or point `--post-file` at that path).
- **`--preprocess-color nv12_to_rgb`** is appropriate for this single-plane RGB-style path (see classification section). Float vstreams: set `"depth_float32": true` in the post JSON.

## Pose / YOLOv8-Pose (`--post-type keypoint`)

Use **`data/yolov8_pose_native_post.example.json`** (or copy it next to the binary as **`./yolov8_pose_native_post.example.json`**).

```bash
./hal-ai-example-v2 --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --profile 0 --model ./yolov8m_pose.hef --post-type keypoint --post-file ./yolov8_pose_native_post.example.json --preprocess-color nv12_to_rgb --udp 192.0.2.93:5555 --verbose
```

## Two-stage OCR

Text detection + recognition (crop then recognize) lives in `hal-ocr-example-v2` under `examples/ocr_example_v2/`. See `examples/ocr_example_v2/readme.md`.
