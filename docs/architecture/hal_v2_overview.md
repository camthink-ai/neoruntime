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
- Dynamic image parameters (rotation, flip, zoom, stabilization)
- Privacy masks (polygon)
- Runtime stream add/remove
- Frontend-to-encoder automatic forwarding control

### AI Inference & GenAI

- `hal_inference.h`: Model inference and post-processing
- `hal_genai.h`: LLM/VLM streaming generation with custom stop words and context management

### DSP (`hal_dsp.h`)

Image processing operations: crop, scale, format conversion, privacy masks, stabilization.

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
