# HAL v2 Overview

## Overview

HAL v2 (`hal_v2/`) is the hardware abstraction layer implementation used by NE503. It features a modular architecture that supports media pipelines, AI inference, DSP processing, and peripheral control.

## Directory Structure

```
hal_v2/
├── include/               # Public headers (organized by module)
│   ├── common/           # Common types, logging, buffers
│   ├── media/            # Media pipeline interface (hal_media.h)
│   ├── model/            # AI model interface (inference, post-processing, GenAI)
│   ├── dsp/              # DSP operation interface
│   └── peripheral/       # Peripheral interface (MCU, devices)
├── platforms/            # Platform implementations
│   ├── hailo15/          # Hailo-15 implementation
│   └── stub/             # Test stub implementation
├── common/               # Common source code
├── examples/             # Example programs
├── third_party/          # Third-party dependencies
└── scripts/              # Build scripts
```

## Interface Model

| Feature | HAL v2 (`hal_v2/`) |
|---------|---------------------|
| Structure | Modular, organized by component directories |
| Interface | Operation tables (Ops structs) |
| Media | Full pipeline (configuration, privacy masks, digital zoom, stabilization) |
| AI | Inference + post-processing + GenAI (LLM/VLM) |
| DSP | Image processing, format conversion, privacy masks |
| Build | CMake supports single library/modular builds |

## Core Interfaces

### Media Pipeline (`hal_media.h`)

Unified video pipeline lifecycle management:
- Profile switching
- Dynamic image parameters (rotation, flip, zoom 1–31x, stabilization)
- Privacy masks (polygon)
- Runtime stream add/remove
- Frontend-to-encoder automatic forwarding control
- Thermal throttling: subscribe to restriction state changes (`subscribe_throttling`) / poll (`get_throttling_state`)
- Motion detection: HAL frame-difference engine on the smallest output stream (`set_motion_config` + transition-triggered `subscribe_motion`) — the medialib module is dormant by design (stock configs disable it; populating its stream_id breaks multi-resize)

### ISP (`hal_isp.h`)

- Image tuning (brightness/contrast/saturation/sharpness), exposure (AE manual/auto), WDR contrast
- Manual white balance gains (`set_wb_config` / `get_current_wb_config`, full awbv2-freeze + enable/mode + Q8.8 gain + optional CCM sequence; visually verified R/B 0.96→4.24 @ R=3.9x)
- Temporal noise reduction (`set_3dnr_config`)
- AE statistics: 256-bin histogram + 5x5 luma grid (`get_ae_stats`, poll model)
- HDR exposure ratios at runtime (`set_hdr_ratios`)
- AF measurement windows + statistics

### Encoder (`hal_codec.h`)

- H.264 / H.265 / MJPEG encoding with CBR / VBR / CVBR / CQP
- ROI / smart encoding (SmartStream+, H.264 + CVBR): `set_roi_config` / `get_roi_config`
- Force intra frame: `force_idr`
- Runtime stream statistics (fps, moving-average bitrate): `get_stream_stats`

### Video capture (`hal_video.h`)

- CSI / FROM_MEDIA capture, dynamic resolution / framerate / format changes
- Sensor module info (SensorRegistry)
- Multi-stage snapshot capture (`request_snapshot` / `list_snapshot_stages`) — pre-ISP raw, post-ISP, per-stream, encoder stages

### AI Inference & GenAI

- `hal_inference.h`: Model inference and post-processing; on-chip NMS parameters at create time (`HalInferenceConfig.nms`) plus a layout-documented NMS decoder (`hal_inference_decode_nms`); session-aware frame preprocessing via `tensor_from_frame_ex` (letterbox / color / resize / normalize — the legacy `tensor_from_frame` stays a raw copy); system stats include SoC on-die temperature (`soc_temp_c` / `soc_temp_c1` via HailoRT `get_chip_temperature`)
- `hal_genai.h`: LLM/VLM streaming generation with custom stop words; context (KV cache) persistence (`save_context` / `load_context` / `get_context_usage`)

### DSP (`hal_dsp.h`)

Image processing operations: crop, scale, format conversion, privacy masks, stabilization; arbitrary-angle rotation (`rotate`), mesh dewarp (`dewarp`), telescopic multi-resize; async job API.

### Peripherals (`hal_mcu.h`)

Generic MCU communication interface for standardized device control.

## Building

```bash
# Build stub (local testing)
make hal-v2

# Build Hailo-15 (requires cross-compilation SDK)
source /opt/poky/4.0.23/environment-setup-armv8a-poky-linux
make hal-v2 HAL_PLATFORM=hailo15
```

## Hailo-15 Implementation

`platforms/hailo15/` contains the complete Hailo-15 platform implementation:

- **Media**: Video capture, encoding (H.264/H.265), ISP, OSD
- **Inference**: HailoRT inference, post-processing, GenAI
- **DSP**: Hailo DSP-based image processing
- **Peripherals**: LED, RTC, sensor, lens control, GPIO

## Examples

The `examples/` directory contains multiple complete examples:

- AI pipeline (multi-model inference)
- GenAI integration
- Dynamic privacy masks
- Two-stage OCR
- Depth estimation, pose detection
- JPEG/MJPEG web verification (`hal-jpeg-web-test`: single-frame + MJPEG stream with built-in HTTP viewer; note JPEG is CPU-encoded on Hailo-15, only H.264/H.265 use the hardware encoder)
