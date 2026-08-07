# NE503 AI Model Performance Benchmark Report

**Device**: Hailo-15H SoC (Integrated NPU, 26 TOPS)
**Firmware**: HailoRT 5.3.0
**Compiler**: HEF Compiler 5.2.0 / 5.3.0
**Test Tool**: `hailortcli benchmark --batch-size 1`
**Test Date**: 2026-05-18
**NPU Config**: max_model_cache=3, exclusive mode (no concurrent models)

---

## 1. Detection Models

### 1.1 YOLOv8n Detection (hailo_yolov8n_384_640)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/detection/hailo_yolov8n_384_640.hef` |
| Size | 4.7 MB |
| Input | NV12, 384×640 (192×640×3 in NV12 layout) |
| Output | YOLOv8 NMS, 4 classes, max 100 bbox/class |
| Contexts | 2 (Multi-Context) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **444.9** |
| **Latency** | **2.25 ms** |
| Temperature | 81.2°C (mean) |

Post-process config: Score threshold 0.20, IoU threshold 0.60, Image 384×640.

### 1.2 YOLOv5m Vehicles (yolov5m_vehicles)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/detection/yolov5m_vehicles.hef` |
| Size | 18 MB |
| Input | F8CR, 1080×1920 (1080×1920×3) |
| Output | YOLOv5 NMS, 1 class (vehicle), max 80 bbox |
| Contexts | 5 (Multi-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **46.6** |
| **Latency** | **21.5 ms** |
| Temperature | 80.0°C (mean) |

Full HD input resolution. Built-in NMS post-process, no external post-process needed.

### 1.3 Tiny YOLOv4 License Plates (tiny_yolov4_license_plates)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/detection/tiny_yolov4_license_plates.hef` |
| Size | 4.7 MB |
| Input | NHWC RGB, 416×416 |
| Output | Raw — 13×13×18 + 26×26×18 (no built-in NMS) |
| Contexts | 1 (Single-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **908.4** |
| **Latency** | **1.10 ms** |
| Temperature | 86.0°C (mean) |

Used as Stage 1 in LPR pipeline. No built-in NMS — external post-process required.

---

## 2. Classification Model

### 2.1 ViT Large (vit_large)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/classification/vit_large.hef` |
| Size | 268 MB |
| Input | NHWC RGB, 224×224 |
| Output | 1×1×1000 (1000-class probabilities) |
| Contexts | 32 (Multi-Context, most complex model) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **18.9** |
| **Latency** | **52.9 ms** |
| Temperature | 83.2°C (mean) |

Largest model by file size. 32 contexts means heavy NPU scheduling overhead.

---

## 3. Segmentation Model

### 3.1 Linknet MobileNetV1 (linknet_mbv1_ss_dpm_256)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/segmentation/linknet_mbv1_ss_dpm_256.hef` |
| Size | 1.9 MB |
| Input | NV12, 256×256 (128×256×3 in NV12 layout) |
| Output | 256×256×2 (2-class segmentation mask) |
| Contexts | 1 (Single-Context) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **1073.2** |
| **Latency** | **0.93 ms** |
| Temperature | 84.6°C (mean) |

Smallest model, highest throughput. Sub-millisecond latency.

---

## 4. Keypoint Model

### 4.1 Face Landmarks Lite (face_landmarks_lite)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/keypoint/face_landmarks_lite.hef` |
| Size | 1.9 MB |
| Input | NV12, 192×96 (96×192×3 in NV12 layout) |
| Output | 1×1×1404 (keypoints) + 1×1×1 (confidence) |
| Contexts | 3 (Multi-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **811.8** |
| **Latency** | **1.23 ms** |
| Temperature | 81.9°C (mean) |

1404 keypoints = 468 points × 3 coordinates (x, y, z). Requires a face detector (yolov8n) running concurrently.

---

## 5. Depth Estimation Model

### 5.1 SCDepthV3 (scdepthv3)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/depth/scdepthv3.hef` |
| Size | 12 MB |
| Input | NHWC RGB, 256×320 |
| Output | 256×320×1 (depth map, UINT16) |
| Contexts | 1 (Single-Context) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **737.3** |
| **Latency** | **1.36 ms** |
| Temperature | 87.5°C (mean) |

Highest observed NPU temperature (89.2°C peak). Monocular depth estimation.

---

## 6. CLIP Zero-Shot Models

### 6.1 CLIP ViT-B/32 NV12 (clip_vit_b_32_image_encoder_nv12)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/clip/clip_vit_b_32_image_encoder_nv12.hef` |
| Size | 83 MB |
| Input | NV12, 224×112 (112×224×3 in NV12 layout) |
| Output | 1×1×512 (image embedding) |
| Contexts | 14 (Multi-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **72.7** |
| **Latency** | **13.8 ms** |
| Temperature | 83.4°C (mean) |

NV12 input variant — zero-copy from camera pipeline.

### 6.2 CLIP ViT-B/16 (clip_vit_b_16_image_encoder)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/clip/clip_vit_b_16_image_encoder.hef` |
| Size | 76 MB |
| Input | NHWC RGB, 224×224 |
| Output | 1×1×512 (image embedding) |
| Contexts | 14 (Multi-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **57.6** |
| **Latency** | **17.4 ms** |
| Temperature | 83.3°C (mean) |

ViT-B/16 has finer patch granularity (16×16) than ViT-B/32 (32×32), slightly slower but more accurate.

---

## 7. OCR Models

### 7.1 LPRNet (lprnet)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/ocr/lprnet.hef` |
| Size | 4.4 MB |
| Input | NHWC RGB, 75×300 |
| Output | 1×19×11 (19 characters × 11 classes) |
| Contexts | 1 (Single-Context) |
| HEF Compiler | 5.2.0 |
| **NPU FPS** | **201.5** |
| **Latency** | **4.96 ms** |
| Temperature | 83.5°C (mean) |

License plate character recognition. 11 classes = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ-".

### 7.2 PaddleOCR v5 Mobile Detection (paddle_ocr_v5_mobile_detection)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/ocr/paddle_ocr_v5_mobile_detection.hef` |
| Size | 5.0 MB |
| Input | NHWC RGB, 544×960 |
| Output | 544×960×1 (text region heatmap) |
| Contexts | 4 (Multi-Context) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **22.5** |
| **Latency** | **44.4 ms** |
| Temperature | 82.6°C (mean) |

Largest input resolution (960×544), heaviest OCR model. Used as Stage 1 in OCR pipeline.

### 7.3 PaddleOCR v5 Mobile Recognition (paddle_ocr_v5_mobile_recognition_nv12)

| Item | Value |
|------|-------|
| File | `/opt/aipc/models/ocr/paddle_ocr_v5_mobile_recognition_nv12.hef` |
| Size | 4.9 MB |
| Input | NV12, 48×320 (24×320×3 in NV12 layout) |
| Output | FCR 1×40×18385 (40 characters × 18385 classes) |
| Contexts | 5 (Multi-Context) |
| HEF Compiler | 5.3.0 |
| **NPU FPS** | **127.4** |
| **Latency** | **7.85 ms** |
| Temperature | 81.2°C (mean) |

NV12 input variant. 18385 classes covers full CJK + Latin character set.

---

## 8. Pipeline Performance

### 8.1 LPR (License Plate Recognition)

```
Camera Frame → tiny_yolov4 (detect plate ROI) → LPRNet (read characters) → Result
```

| Stage | Model | NPU FPS | Latency |
|-------|-------|---------|---------|
| 1. Detection | tiny_yolov4_license_plates | 908 | 1.10 ms |
| 2. Recognition | lprnet | 201 | 4.96 ms |
| **Pipeline Total** | Serial + ROI crop | **~30 FPS** (est.) | **~6 ms + overhead** |

Bottleneck: ROI extraction and per-plate recognition when multiple plates are detected.

### 8.2 OCR (Text Recognition)

```
Camera Frame → paddle_det (detect text regions) → paddle_rec (recognize text) → Result
```

| Stage | Model | NPU FPS | Latency |
|-------|-------|---------|---------|
| 1. Detection | paddle_ocr_v5_mobile_detection | 22.5 | 44.4 ms |
| 2. Recognition | paddle_ocr_v5_mobile_recognition | 127 | 7.85 ms |
| **Pipeline Total** | Serial + ROI crop | **~18 FPS** (est.) | **~52 ms + overhead** |

Bottleneck: Stage 1 detection (44.4 ms). Stage 2 runs per text region.

---

## 9. Performance Summary

### Throughput Ranking (NPU FPS, descending)

| Rank | Model | Type | Input | FPS | Latency | Size |
|------|-------|------|-------|-----|---------|------|
| 1 | linknet_mbv1_ss_dpm_256 | Segmentation | 256×256 | **1073** | 0.93 ms | 1.9M |
| 2 | tiny_yolov4_license_plates | Detection | 416×416 | **908** | 1.10 ms | 4.7M |
| 3 | face_landmarks_lite | Keypoint | 192×96 | **812** | 1.23 ms | 1.9M |
| 4 | scdepthv3 | Depth | 256×320 | **737** | 1.36 ms | 12M |
| 5 | hailo_yolov8n_384_640 | Detection | 384×640 | **445** | 2.25 ms | 4.7M |
| 6 | lprnet | OCR Rec | 75×300 | **202** | 4.96 ms | 4.4M |
| 7 | paddle_recognition | OCR Rec | 48×320 | **127** | 7.85 ms | 4.9M |
| 8 | clip_vit_b_32 | CLIP | 224×112 | **73** | 13.8 ms | 83M |
| 9 | clip_vit_b_16 | CLIP | 224×224 | **58** | 17.4 ms | 76M |
| 10 | yolov5m_vehicles | Detection | 1080×1920 | **47** | 21.5 ms | 18M |
| 11 | paddle_detection | OCR Det | 544×960 | **22** | 44.4 ms | 5.0M |
| 12 | vit_large | Classification | 224×224 | **19** | 52.9 ms | 268M |

### Performance Tiers

| Tier | FPS Range | Models | Use Case |
|------|-----------|--------|----------|
| Ultra-high | >500 FPS | linknet, tiny_yolov4, face_landmarks, scdepthv3, yolov8n | Real-time multi-stream, low-latency |
| Medium | 20-200 FPS | yolov5m, lprnet, paddle_rec, clip_b_32, clip_b_16 | Single-stream real-time |
| Heavy | <25 FPS | paddle_det, vit_large | Batch processing, non-real-time |

### NPU Utilization Notes

- **Contexts** indicate NPU scheduling complexity. More contexts = higher scheduling overhead but enables model interleaving.
- **max_model_cache=3** allows up to 3 models loaded simultaneously. Model switching has ~10-50ms overhead.
- **Temperature**: All tests ran at 80-89°C. SCDepthV3 peaked at 89.2°C (close to thermal throttle at 90°C).
- **Real-world FPS** is typically 60-80% of NPU benchmark due to video decode, pre/post-process, and memory copy overhead.
