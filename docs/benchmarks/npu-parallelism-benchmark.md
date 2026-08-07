# Hailo-15H NPU Model Parallelism Benchmark

> **Test Date**: 2025-05-25
> **Device**: NE503 (AM20-01) · Hailo-15H SoC · 8 GB LPDDR4X
> **HailoRT**: v5.3.0
> **ai-runtime Configuration**: `global_concurrent_limit: 8`, `queue_size: 64`
> **Note**: YAML parameters `max_model_cache`, `memory_limit_mb`, `strategy`, etc. are **not implemented in code** and do not affect actual behavior (see [ai-runtime-performance-params.md](./ai-runtime-performance-params.md) for details)

---

## 1. Hardware Specifications

| Item | Value |
|------|-------|
| SoC | Hailo-15H (HAILO15H) |
| NPU Compute | **20 TOPS** (INT8) |
| NPU Architecture | Hailo Structure-Defined Dataflow |
| CPU | 4x Cortex-A53 @ 1.3 GHz (12k DMIPS) |
| DSP | Vector DSP, 256 MACs @ 700 MHz (350 GOPs) |
| Memory | LPDDR4X 32-bit @ 4266 MT/s, **68.3 GB/s** bandwidth |
| Memory Capacity | 8 GB (this device) |
| Power | < 5W |
| ISP | Dual ISP, 12 MP, 600 Mpixel/s |

## 2. HailoRT Software Limits

Source `hailort/libhailort/include/hailo/hailort.h`:

```c
#define HAILO_MAX_NETWORK_GROUPS           (8)   // Max simultaneously loaded network groups
#define HAILO_MAX_STREAMS_COUNT            (40)  // Max data streams
#define HAILO_MAX_NETWORKS_IN_NETWORK_GROUP (8)  // Max sub-networks per group
```

**Key Constraints**:

- A single VDevice can simultaneously activate up to **8 network groups** (models)
- Each network group can have up to **8 sub-networks**
- Total data stream limit of **40**
- NPU internally time-shares NN Core in units of **Contexts**

## 3. Model Context Consumption

Contexts are the basic scheduling unit of the NPU. Each HEF model is compiled into a number of Contexts; all loaded models' Contexts share NN Core time slices.

| Model | Contexts | Input Size | Type | Complexity |
|-------|----------|------------|------|------------|
| yolov8n (384x640) | 2 | NV12 192x640 | Detection | Small |
| face_landmarks_lite | 2 | RGB 192x192 | Keypoints | Small |
| lprnet | 1 | -- | OCR recognition | Small |
| tiny_yolov4_license_plates | 1 | -- | Detection | Small |
| scdepthv3 | 1 | RGB 256x320 | Depth estimation | Small |
| linknet_mbv1_ss_dpm_256 | 1 | -- | Segmentation | Small |
| yolov5m_vehicles | 4 | -- | Detection | Medium |
| paddle_ocr_v5_det | 4 | -- | OCR detection | Medium |
| paddle_ocr_v5_rec | 5 | -- | OCR recognition | Medium |
| clip_vit_b_32 | 14 | NV12 112x224 | CLIP encoding | Large |
| vit_large | 32 | -- | General | Large |
| Qwen3-VL-2B (prefill) | 140 | -- | VLM | Very Large |
| Qwen3-VL-2B (tbt) | 138 | -- | VLM | Very Large |
| Qwen3-VL-2B (vision) | 92 | -- | VLM | Very Large |

**Total Context count directly affects parallel inference latency** -- more Contexts means higher scheduling overhead per full inference round.

## 4. Benchmark Results

Test environment: Inside model-showcase container, using `InferenceClient` to call ai-runtime via gRPC.

### 4.1 Single Model Sequential Inference (Baseline)

Each model has exclusive NPU access, tested one at a time.

| Model | Avg Latency | P50 | P95 | Min | Max | N |
|-------|------------|-----|-----|-----|-----|---|
| YOLOv8n | 21.6 ms | 20.3 ms | 34.5 ms | 14.3 ms | 36.3 ms | 30 |
| FaceLandmarks | 9.8 ms | 9.2 ms | 13.8 ms | 7.2 ms | 14.8 ms | 30 |
| SCDepth | 41.6 ms | 41.6 ms | 49.9 ms | 33.7 ms | 56.9 ms | 30 |

### 4.2 Dual Model Parallel

YOLOv8n + FaceLandmarks loaded simultaneously, dual-thread concurrent inference.

| Model | Sequential Latency | Parallel Latency | **Latency Increase** |
|-------|-------------------|------------------|---------------------|
| YOLOv8n | 21.4 ms | 29.5 ms | **+38%** |
| FaceLandmarks | 9.5 ms | 19.4 ms | **+104%** |

| Metric | Value |
|--------|-------|
| Parallel throughput | **73.2 inf/s** |
| Equivalent serial throughput | ~31.5 inf/s (1/(21+10)ms) |
| **Parallel speedup** | **2.3x** |

### 4.3 Triple Model Parallel

YOLOv8n + FaceLandmarks + SCDepth loaded simultaneously, triple-thread concurrent inference.

| Model | Sequential Latency | Parallel Latency | **Latency Increase** |
|-------|-------------------|------------------|---------------------|
| YOLOv8n | 22.0 ms | 36.0 ms | **+64%** |
| FaceLandmarks | 10.5 ms | 24.7 ms | **+135%** |
| SCDepth | 40.3 ms | 59.2 ms | **+47%** |

| Metric | Value |
|--------|-------|
| Parallel throughput | **49.8 inf/s** |
| Equivalent serial throughput | ~13.7 inf/s (1/(22+10+41)ms) |
| **Parallel speedup** | **3.6x** |

### 4.4 Quad Model Parallel

Loaded a 4th model (CLIP ViT-B/32) to verify feasibility of 4 simultaneous models.

| Result | Description |
|--------|-------------|
| Registration | All 4 models registered successfully |
| Inference | All 4 models inferred successfully |
| YOLOv8n | 26.4 ms |
| FaceLandmarks | 10.8 ms |
| SCDepth | 45.9 ms |
| CLIP ViT-B | 29.6 ms |

> YAML `max_model_cache: 3` is not implemented in code; model registration count is not limited by this. The actual constraint comes from HailoRT's `HAILO_MAX_NETWORK_GROUPS=8` and physical memory.

## 5. Theoretical Limit Analysis

### 5.1 Constraint Dimensions

| Dimension | Hard Limit | Source |
|-----------|-----------|--------|
| Network groups | **8** | `HAILO_MAX_NETWORK_GROUPS` (HailoRT header) |
| Total data streams | **40** | `HAILO_MAX_STREAMS_COUNT` (HailoRT header) |
| Physical memory | **8 GB DDR** | Shared (CPU + NPU have no dedicated VRAM) |
| NN Core compute | **20 TOPS** | Hardware |

> Note: YAML `memory_limit_mb: 2048` and `max_model_cache: 3` are **not implemented in code** and do not affect actual limits. Real constraints are HailoRT's 8 network group cap and 8 GB physical memory.

### 5.2 Practical Capacity Estimation

Using model combinations on the current device (constraints: HailoRT max 8 network groups, 8 GB DDR shared):

| Scenario | Model Combination | Total Contexts | Feasibility |
|----------|------------------|----------------|-------------|
| Lightweight x 6 | yolov8n + landmarks + lprnet + plate_det + scdepth + linknet | 2+2+1+1+1+1 = 8 | Feasible |
| Lightweight x 3 + Medium x 1 | yolov8n + landmarks + scdepth + yolov5m | 2+2+1+4 = 9 | Feasible |
| Lightweight x 2 + Large x 1 | yolov8n + landmarks + clip_vit_b_32 | 2+2+14 = 18 | Feasible |
| Large x 2 | clip_vit_b_32 + vit_large | 14+32 = 46 | Near limit |
| VLM | Qwen3-VL-2B | 370 | Requires exclusive access, no parallelism |

> The above feasibility is theoretical estimation. HailoRT's `VDevice::create_infer_model()` checks resources when models are actually loaded onto the NPU, and returns an error code if hardware capacity is exceeded.

### 5.3 Latency vs Parallelism Trend

```
Latency (ms)
  80 +                                        +-- SCDepth
  60 +                              +---------+
  40 +               +--------------+
  30 +     +---------+  YOLOv8n
  20 +-----+
  10 +-- FaceLandmarks
   0 +------+-------+-------+-------+-------+
        1 model  2 model  3 model  4 model  8 model
       (exclusive) (parallel) (parallel) (exceeded) (theoretical)
```

**Patterns**:

- Small models are more affected by parallelism (100%+ latency increase), because their own inference is fast and scheduling wait time accounts for a higher proportion
- Large models are less affected by parallelism (~50% latency increase), because inference itself accounts for most of the time
- Parallel throughput is always better than serial (**3-model parallel throughput is 3.6x serial**)

## 6. Parallelism Capability Summary

### 6.1 Actually Active Constraints

| Constraint | Real Limit | Source |
|-----------|-----------|--------|
| Simultaneously loaded models | **8** | HailoRT `HAILO_MAX_NETWORK_GROUPS` |
| Physical memory | **8 GB** | DDR shared, no dedicated VRAM |
| Worker threads | **8** | ai-runtime `global_concurrent_limit` |
| Request queue | **64** | ai-runtime `queue_size` |

### 6.2 Inactive Configuration (Reserved in code, not implemented)

The following parameters exist in YAML but are not consumed by code; modifying them has no effect:

- `max_model_cache` -- Does not limit model registration count
- `memory_limit_mb` -- Does not limit model memory usage
- `strategy` -- Scheduling strategy is fixed to Round-Robin Fair Queueing
- `device_mode` -- Not passed to HailoRT
- `batch_*` -- Batching not implemented
- QPS/priority/timeout -- All not implemented

See [ai-runtime-performance-params.md](./ai-runtime-performance-params.md) for details.

### 6.3 Recommended Parallelism Count by Scenario

| Scenario | Recommended Model Count | Description |
|----------|------------------------|-------------|
| Smart security (detection + keypoints) | 2-3 | Current model-showcase typical configuration |
| Multi-model analysis (detection + depth + segmentation + OCR) | 4-5 | Primarily small models, latency manageable |
| Lightweight full parallel (6 small models) | 6 | Total context=8, near but not at HailoRT limit |
| With CLIP/ViT or other large models | 2-3 | Large models have high context overhead (14-32), crowding scheduling |

## 7. Summary

| Metric | Value | Source |
|--------|-------|--------|
| NPU compute | 20 TOPS (INT8) | Hardware |
| HailoRT max network groups | **8** | `HAILO_MAX_NETWORK_GROUPS` hardcoded |
| Physical memory | **8 GB DDR** (CPU + NPU shared) | Hardware |
| Small model theoretical max parallelism | **5-6** (constrained by HailoRT 8 network groups + DDR) | Estimated |
| With large models recommended parallelism | **2-3** | Measured |
| 3-model parallel throughput | **49.8 inf/s** (3.6x serial 13.7 inf/s) | Measured |
| Best single-model latency | 9.8 ms (FaceLandmarks) | Measured |
| 3-model parallel max latency | 59.2 ms (SCDepth, still ~17 FPS) | Measured |

**Core conclusion**: The real constraints on NPU parallelism are HailoRT's 8 network group cap and 8 GB physical memory. YAML `max_model_cache` and `memory_limit_mb` are **not implemented in code** and do not affect actual behavior.

---