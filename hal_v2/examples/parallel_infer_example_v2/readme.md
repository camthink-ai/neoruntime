# parallel_infer_example_v2

Demonstrates **N-model parallel inference** on Hailo-15 using the shared HAL runtime (`runtime_acquire`) and HailoRT Model Scheduler (Round-Robin).

## What it does

1. Opens a medialib camera pipeline (JSON profile).
2. Acquires one shared `HalInferenceRuntime` and loads **N HEF models** onto it.
3. **Pipelined** processing (not serial per frame):
   - Video frames are queued and submitted as soon as a per-model pipeline buffer is free.
   - For each frame tick, all models get `run_async` without waiting for the previous frame to finish.
   - Each model owns a **buffer pool** (`--pipeline-depth`, default 8) so concurrent jobs never share input/output tensors.
4. Prints periodic stats:
   - System NPU / CPU / RAM / DSP utilization
   - **frame_fps**: camera frames where **all** models finished (see `done=` counter)
   - Per-model **model_fps** (async completions/s), **pre_ms**, **e2e_ms** (run_async→callback, includes scheduler queue wait + NPU — not pure NPU infer time), ok/err, pending depth

## Build

From `hal_v2/` with the Hailo SDK environment sourced:

```bash
source ../sdk_4.0.23/environment-setup-armv8a-poky-linux
cmake -B build-hailo15 -DHAL_PLATFORM=hailo15
cmake --build build-hailo15 --target hal-parallel-infer-example-v2 -j$(nproc)
```

Binary: `build-hailo15/hal-parallel-infer-example-v2`

## Usage

```bash
export HAILO_MONITOR=1   # optional: enable hailortcli-style monitor on device

./hal-parallel-infer-example-v2 \
  /path/to/frontend.json \
  /opt/aipc/models/yolov8n.hef \
  /opt/aipc/models/scdepth.hef \
  --frames 500 \
  --pipeline-depth 8 \
  --letterbox \
  --scheduler-threshold 4 \
  --scheduler-timeout-ms 100 \
  --priority 0:24 \
  --stats-interval-ms 1000
```

### Arguments

| Argument | Description |
|----------|-------------|
| `medialib_json` | Hailo medialib profile (same as other HAL v2 examples) |
| `hef1 … hefN` | Up to 8 model paths |
| `--frames N` | Stop after N completed frames (0 = until Ctrl+C) |
| `--video-index I` | Video frontend index (default: 0). Any resolution works — DSP resizes to each model input; a smaller stream (e.g. 640×384) reduces DSP load. |
| `--pipeline-depth N` | In-flight buffer slots **per model** (default 8). Should be ≥ scheduler threshold for best throughput. |
| `--letterbox` | Preserve aspect ratio with padding (default: stretch) |
| `--scheduler-threshold N` | HailoRT scheduler threshold per model (default **4** — batch frames before NPU run) |
| `--scheduler-timeout-ms N` | Scheduler wait cap when threshold is not met (default **100** ms) |
| `--priority idx:prio` | Set scheduler priority for model index (repeatable) |
| `--stats-interval-ms N` | Stats table print interval |

## Notes

- The example calls `HAL_MEDIA_OPS.start()` after `subscribe_stream` (same as `ai_example_v2`). If stats show `frames in=0`, the pipeline is not running — check camera/medialib config and that no other app holds the sensor exclusively.
- Ensure `hailort_server.service` is running if multiple processes share the NPU (`multi_process_service=true` by default).
- **Throughput vs latency:** this example is meant to stress **parallel multi-model throughput**. Use `scheduler_threshold` ≥ 2 and `pipeline_depth` ≥ threshold so the scheduler can batch frames. For minimum single-frame latency, use `ai_example_v2` or set `--scheduler-threshold 1 --pipeline-depth 2`.
- Models with different input sizes each get an independent resize buffer per pipeline slot; inference is scheduled concurrently on one VDevice.
- **Preprocess (HailoDSP):** camera frames are **CPU-copied** into an app-pool NV12 scratch buffer first (frontend DMA cannot mix with request-pool DMA in one DSP op). Then HailoDSP resizes once to `max(model W)×max(model H)` and per-model, with `convert_format` for RGB models. Frontend buffers are released immediately after preprocess (before async infer). NPU inputs (`nv12_blob`, `rgb_packed`) are tight-packed heap vectors. All DSP ops are serialized on `dsp_mu`.
- **CMA:** resize scratch buffers use `HAL_MEM_DMABUF` with pooled `pool_max` budgeted per geometry. If dmesg shows `cma_alloc … -12`, lower `--pipeline-depth`, use a smaller `--video-index`, or reduce model count.
- For RGB models, NV12→RGB conversion uses HailoDSP after resize.
