# AI Runtime Service

## Overview

`ai-runtime` is the unified inference service responsible for NPU compute scheduling, model lifecycle management, and inference execution. It supports traditional CV model inference and GenAI (LLM/VLM) streaming generation.

Tech stack: C++17 + gRPC, dynamically loads hardware acceleration libraries through HAL.

## Architecture Diagram

```mermaid
flowchart TD
    subgraph "Application Layer"
        A[Client Application] -->|gRPC| B[AI Runtime]
        C[Model Showcase] -->|gRPC| B
    end

    subgraph "AI Runtime Core"
        B -->|Manage| C1[ModelManager]
        B -->|Quota Management| C2[SessionManager]
        B -->|Scheduling| C3[InferenceScheduler]
        B -->|Zero-copy| C4[FdReceiver]
        B -->|Auto Inference| C5[AutoInfer]
    end

    subgraph "HAL Abstraction"
        C1 -->|Load/Unload| H1[HalInferenceOps]
        C1 -->|Post-process| H2[HalPostprocessOps]
        C1 -->|Draw| H3[HalDrawOps]
        C1 -->|CLIP Encoding| H4[HalClipTextEncoderOps]
        C1 -->|GenAI| H5[HalGenaiOps]
    end

    subgraph "Hardware Layer"
        H1 -->|DMA-BUF| NPU[Hailo-15 NPU]
        H2 -->|NMS/Decoding| NPU
        H3 -->|Video Draw| NPU
        H4 -->|Text Encoding| NPU
        H5 -->|LLM/VLM| NPU
    end

    subgraph "External Services"
        C4 -->|SCM_RIGHTS| D1[camera-daemon]
        C5 -->|Subscribe| D1
        B -->|Publish| D2[Event Bus]
        C1 -->|Dynamic Load| D3[libaipc_hal.so]
    end

    A -->|Unix Socket| B
    B -->|Event Publish| D2
```

## Directory Structure

```
platform/ai-runtime/
├── include/                    # Header files
│   ├── config.h               # Configuration struct definitions
│   ├── grpc_service.h         # gRPC service implementation
│   ├── model_manager.h        # Model lifecycle management
│   ├── session_manager.h      # Session quota management
│   ├── inference_scheduler.h  # Inference task scheduling
│   ├── hal_ml_loader.h        # HAL ML library loader
│   ├── fd_receiver.h          # DMA-BUF zero-copy receiver
│   ├── event_bus_client.h     # Event bus client
│   ├── auto_infer.h           # Auto inference pipeline
│   ├── common.h               # Common definitions and types
│   ├── log.h                  # Logging interface
│   └── fd_protocol.h          # FD transfer protocol definitions
├── src/                       # Source files
│   ├── main.cpp               # Entry point
│   ├── grpc_service.cpp       # gRPC service (1716 lines)
│   ├── model_manager.cpp      # Model management (576 lines)
│   ├── session_manager.cpp    # Session management (163 lines)
│   ├── inference_scheduler.cpp# Scheduler (333 lines)
│   ├── hal_ml_loader.cpp      # HAL integration
│   ├── fd_receiver.cpp        # Zero-copy (351 lines)
│   ├── event_bus_client.cpp   # Event publishing
│   └── auto_infer.cpp         # Auto inference (581 lines)
├── proto/
│   └── inference.proto        # gRPC definitions
├── test/                      # Tests
└── build-*/                   # Build artifacts
```

## gRPC API

Service name: `InferenceService`, listening on `unix:///run/aipc/ai-runtime.sock`.

### Model Management

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `RegisterModel` | `ModelRegisterRequest` | `ModelRegisterResponse` | Register model to NPU |
| `UnregisterModel` | `ModelInfo` | `Status` | Unload model |
| `ListModels` | `Empty` | `ModelListResponse` | List registered models |
| `GetModelInfo` | `ModelInfo` | `ModelInfo` | Get model details |

### Inference

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `Infer` | `InferRequest` | `InferResponse` | Single inference |
| `InferBatch` | `InferBatchRequest` | `InferBatchResponse` | Batched inference |
| `StreamInfer` | `StreamInferRequest` | `stream StreamInferResponse` | Streaming inference (real-time video) |

### Session Management

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `CreateSession` | `SessionConfig` | `SessionCreateResponse` | Create quota session |
| `DestroySession` | `SessionConfig` | `Status` | Destroy session |

### Statistics and Configuration

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `GetStats` | `Empty` | `SystemStats` | Get system statistics |
| `UpdatePostprocessConfig` | `UpdatePostprocessConfigRequest` | `UpdatePostprocessConfigResponse` | Update post-processing config |
| `EncodeText` | `EncodeTextRequest` | `EncodeTextResponse` | CLIP text encoding |

### GenAI (LLM/VLM)

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `GenaiCreateSession` | `GenaiCreateSessionRequest` | `GenaiCreateSessionResponse` | Create GenAI session |
| `GenaiDestroySession` | `GenaiCreateSessionRequest` | `Status` | Destroy session |
| `GenaiGenerate` | `GenaiGenerateRequest` | `stream GenaiGenerateResponse` | Streaming generation |
| `GenaiAbort` | `GenaiAbortRequest` | `Status` | Abort generation |

### Key Message Structures

**Tensor Data**:
```protobuf
message Tensor {
  repeated int32 shape = 1;
  DataType dtype = 2;     // UINT8/INT8/FLOAT16/FLOAT32 etc.
  bytes data = 3;
  int32 dma_fd = 4;       // DMA-BUF zero-copy file descriptor
}
```

**Structured Post-processing Results**:
```protobuf
message PostResult {
  repeated Detection detections = 1;       // Object detection
  repeated Classification classifications = 2;  // Classification
  repeated LandmarkSet landmarks = 3;      // Keypoints
  repeated SegmentationMask masks = 4;     // Segmentation
  repeated OcrLine ocr_lines = 5;          // OCR
  repeated Embedding embeddings = 6;       // Embeddings
  repeated DepthMap depth_maps = 7;        // Depth estimation
}
```

**Model Registration Request**:
```protobuf
message ModelRegisterRequest {
  string model_path = 1;
  string model_id = 2;
  repeated TensorSpec inputs = 3;
  repeated TensorSpec outputs = 4;
  string model_type = 13;      // detection, landmarks, segmentation, classification
  string model_variant = 14;   // yolov8n, yolov8s etc.
  string owner_id = 12;
}
```

## Configuration

Configuration file: `configs/ai/ai-runtime.yaml`

```yaml
service:
  name: ai-runtime
  listen: unix:///run/aipc/ai-runtime.sock
  log_level: debug

hal:
  library_path: /data/aipc/lib/hal/libaipc_hal.so
  device_path: /dev/hailo0

models:
  repository_path: /data/aipc/models
  cache_path: /var/cache/aipc/models
  preload: []

scheduler:
  global_qps_limit: 100
  global_concurrent_limit: 8
  default_session:
    max_qps: 30
    max_concurrent: 2
    priority: 5
  strategy: fair          # priority | fifo | fair
  queue_size: 64
  timeout_ms: 5000

fd_receiver:
  socket_path: /run/aipc/camera.sock

performance:
  device_mode: high       # high | normal | low
  batch_enabled: false
  batch_size: 1
  batch_timeout_ms: 100
  max_model_cache: 3
  memory_limit_mb: 2048

monitoring:
  enabled: true
  stats_interval_sec: 10
  metrics_port: 9090
  temperature_limit_c: 85
  throttle_temperature_c: 80

event_bus:
  enabled: true
  endpoint: unix:///run/aipc/event-bus.sock
  auto_publish_results: true
  result_topic_prefix: "inference/"

auto_infer:
  enabled: false
  pipelines:
    - model_id: "yolov8n_det"
      stream_id: "cam1"
      fps: 10
    - model_id: "clip_text_encoder"
      stream_id: "cam2"
      fps: 5
```

## Core Component Implementation Details

### 1. Complete Inference Pipeline Flow

```mermaid
flowchart TD
    A[Client gRPC Request] --> B[SessionManager Check Quota]
    B -->|OK| C[ModelManager Get Model Snapshot]
    B -->|Quota Exceeded| D[Return Error]

    C --> E[InferenceScheduler Task Queue]
    E --> F[Round-Robin Select Active Session]
    F --> G[N Worker Thread Pool]

    G --> H[HAL Inference Session]
    H -->|Zero-copy| I[DMA-BUF Input Tensor]
    H -->|Direct Execution| J[NPU Hardware Inference]
    J --> K[Raw Output Tensor]
    K --> L[Optional Post-processing]

    L -->|Detection/Classification/Segmentation| M[PostProcess Results]
    L -->|No Post-processing| N[Raw Tensor]

    M --> O[HAL Memory Management]
    N --> O

    O --> P[Callback Return Results]
    P --> Q[Event Bus Publish]
    Q --> R[camera-daemon Draw]
    P --> S[Client Response]

    style H fill:#f9f,stroke:#333,stroke-width:2px
    style J fill:#f9f,stroke:#333,stroke-width:2px
```

### 2. Model Lifecycle State Diagram

```mermaid
stateDiagram-v2
    [*] --> Unregistered: Model not loaded

    Unregistered --> Registering: register_model()
    Registering --> Ready: HAL loaded successfully
    Registering --> Unregistered: Load failed

    Ready --> Active: acquire_model_snapshot()
    Ready --> Unregistering: unregister_model()
    Active --> Ready: release_model()
    Active --> Unregistering: ref_count=0

    Unregistering --> Unregistered: HAL session destroyed successfully
    Unregistering --> Ready: Still has references/owners

    Active --> Registered: co-ownership added
    Ready --> Registered: co-ownership added

    state "Co-ownership" as CO {
        [*] --> Owner: Add owner_id
        Owner --> Owner: Add new owner
        Owner --> Unowner: Remove owner
        Unowner --> [*]: All owners cleared
    }

    Registered --> Owner: register_model(owner_id)
    Owner --> Registered: Owners > 1
    Owner --> Ready: Last owner left
```

**ModelManager Key Features**:
- **Reference Counting**: `ref_count` tracks active usage
- **Co-ownership**: Multiple applications can share models, unloaded per owner
- **Atomic Snapshots**: `ModelSnapshot` ensures thread safety
- **RAII Protection**: `ModelGuard` automatically releases references

### 3. Scheduler Round-Robin Algorithm

```mermaid
flowchart TD
    A[Submit task to session_queues] --> B[Check total queue capacity]
    B -->|Not full| C[Add to corresponding Session queue]
    B -->|Full| D[Drop task]

    C --> E[Worker threads waiting]

    subgraph "Round-Robin Scheduling"
        F[Condition variable wakeup] --> G[rr_index points to current Session]
        G --> H[Check active_sessions list]
        H --> I[Dequeue front task from that Session]
        I --> J[Update rr_index]
        J --> K[Remove Session if queue empty]
        K --> L[If not empty, rr_index++]
    end

    L --> M[Model snapshot acquired]
    M --> N[Inference execution]
    N --> O[Result callback]

    subgraph "Session Queue Management"
        Q1[Session A Queue] -->|Task| C
        Q2[Session B Queue] -->|Task| C
        Q3[Session C Queue] -->|Task| C
        active_sessions --> LIST["[A, B, C]"]
    end

    style N fill:#f9f,stroke:#333,stroke-width:2px

    ```

**Scheduler Features**:
- **Fair Scheduling**: Round-Robin algorithm ensures fairness
- **Session Isolation**: Each session has an independent queue
- **Capacity Control**: Global queue limit prevents memory overflow
- **Dynamic Load Balancing**: Automatic active session detection

### 4. DMA-BUF Zero-Copy Flow

```mermaid
sequenceDiagram
    participant C as camera-daemon
    participant F as FdReceiver
    participant S as Subscriber1
    participant S2 as Subscriber2
    participant S3 as Subscriber3

    Note over C,S3: Phase 1: Subscription Establishment

    S->>F: subscribe(cam1, sub1, callback)
    F->>C: Connect /run/aipc/camera.sock
    F->>C: Send SUBSCRIBE
    C-->>F: OK + SCMs (DMA-BUF FDs)
    F->>F: Start recv_thread
    Note over F: Physical connection established

    S2->>F: subscribe(cam1, sub2, callback)
    F-->>S2: Reuse connection
    Note over F: Add to subscribers list

    S3->>F: subscribe(cam1, sub3, callback)
    F-->>S3: Reuse connection
    Note over F: subscribers = [sub1, sub2, sub3]

    Note over C,S3: Phase 2: Frame Reception and Dispatch

    loop Every frame
        C->>F: fd_pub_sendmsg(FRAME + 3 FDs)
        F->>F: recv_loop processing
        F->>F: ref_count = 3 (multiple subscribers)

        par Parallel dispatch
            F->>S: callback(frame1)
            F->>S2: callback(frame1)
            F->>S3: callback(frame1)
        end

        Note over S,S3: Each subscriber gets the same frame_fd_group
    end

    Note over C,S3: Phase 3: Release Mechanism

    S->>F: release_frame(cam1, fid=123)
    F->>F: ref_count-- = 2
    F->>F: Do not send RELEASE

    S2->>F: release_frame(cam1, fid=123)
    F->>F: ref_count-- = 1
    F->>F: Do not send RELEASE

    S3->>F: release_frame(cam1, fid=123)
    F->>F: ref_count-- = 0
    F->>C: fd_pub_sendmsg(RELEASE)

    Note over C,S3: Release back to camera-daemon buffer pool
```

**FdReceiver Key Features**:
- **Connection Reuse**: Multiple subscribers share physical connection
- **Reference Counting**: Prevents premature DMA-BUF release
- **Multicast**: All subscribers receive the same frame
- **RAII Management**: `FdGroup` automatically closes FDs

### 5. Auto Inference Pipeline

```mermaid
flowchart TD
    A[Camera Frame Input] --> B[FdReceiver Subscribe]
    B --> C[DMA-BUF Mapping]
    C --> D[Input Pre-processing]

    subgraph "Pre-processing Branches"
        D -->|CLIP Model| E[NV12->RGB Resize]
        D -->|Other Models| F[NV12 Scale/Copy]
    end

    E --> G[Build HalTensor]
    F --> G

    G --> H[Submit Task to Scheduler]
    H --> I[MAX_IN_FLIGHT=3 Check]

    I -->|Not full| J[Enqueue]
    I -->|Full| K[Drop and Release]

    J --> L[Inference Execution]
    L --> M[Post-processing]
    M --> N[Publish Event Bus]
    N --> O[camera-daemon Draw]

    subgraph "Post-processing"
        M -->|Detection| P[Non-Maximum Suppression]
        M -->|Classification| Q[Top-K Selection]
        M -->|Segmentation| R[Mask RLE Encoding]
        M -->|CLIP| S[Embedding Vector Extraction]
    end

    K --> C

    style N fill:#f9f,stroke:#333,stroke-width:2px
```

**AutoInfer Features**:
- **MAX_IN_FLIGHT=3**: Prevents camera buffer pool exhaustion
- **Adaptive Pre-processing**: Converts format based on model type
- **Diverse Post-processing**: Supports detection/classification/segmentation/CLIP
- **Flow Control**: FPS limiting and frame dropping

### 6. Session ID Format and Quota Management

```mermaid
graph TD
    A[Session ID Generation] --> FORMAT["{app_id}-{stream_id}-{model_id}-{timestamp}"]

    subgraph "Quota Check"
        B[FPS Limit] -->|check_fps_limit| C[Time Interval Validation]
        D[QPS Limit] -->|check_qps_limit| E[Sliding Window Average]
        F[Concurrency Limit] -->|max_concurrent| G[Current Active Count]
    end

    C -->|Pass| H[Allow Inference]
    E -->|Pass| H
    G -->|Not Exceeded| H

    H --> I[record_inference]
    I --> J[Update Statistics]

    subgraph "Session Configuration"
        K[Default Quota] -->|max_qps: 30, max_concurrent: 2| L[Applied]
        M[High Priority] -->|priority: 7, max_qps: 50| N
        O[Low Priority] -->|priority: 1, max_qps: 10| P
    end

    style C fill:#f9f,stroke:#333,stroke-width:2px
    style E fill:#f9f,stroke:#333,stroke-width:2px
```

### 7. Post-processing Type Mapping Table

| Post-processing Type | HAL Enum | Model Type | Default Parameters |
|---------------------|----------|------------|-------------------|
| Detection | `HAL_POST_TYPE_DETECTION` | detection, yolo | Confidence 0.25, NMS 0.45 |
| Keypoints | `HAL_POST_TYPE_KEYPOINT` | landmarks, keypoint | Confidence 0.25 |
| Segmentation | `HAL_POST_TYPE_SEGMENTATION` | segmentation | Confidence 0.25 |
| Classification | `HAL_POST_TYPE_CLASSIFICATION` | classification | Confidence 0.25, Top-K 5 |
| CLIP | `HAL_POST_TYPE_CLIP` | clip, embedding | Default config |
| OCR Detection | `HAL_POST_TYPE_OCR_DETECTION` | ocr_detection | Confidence 0.25, NMS 0.45 |
| OCR Recognition | `HAL_POST_TYPE_OCR_RECOGNITION` | ocr_recognition | Default config |
| Depth Map | `HAL_POST_TYPE_DEPTH` | depth, monocular_depth | Default config |

## HAL Integration

Dynamically loads `libaipc_hal.so` via `HalMlLoader`, using the following HAL interfaces:

| HAL Interface | Function | Key Methods |
|---------------|----------|-------------|
| `HalInferenceOps` | Core inference operations | `create`, `run`, `destroy` |
| `HalPostprocessOps` | Post-processing (NMS, decoding, etc.) | `create`, `run`, `apply_config_json` |
| `HalDrawOps` | Drawing/visualization | `overlay_detection` etc. |
| `HalClipTextEncoderOps` | CLIP text encoding | `create`, `encode`, `destroy` |
| `HalGenaiOps` | GenAI LLM/VLM operations | `create`, `generate_stream`, `abort_generation` |

## Performance Optimization Features

### 1. Zero-Copy Optimization
- **DMA-BUF Transfer**: Zero memory copy in camera-to-AI pipeline
- **File Descriptor Sharing**: Pass FDs via SCM_RIGHTS
- **Memory Mapping**: `MappedNV12Frame` automatically manages mappings

### 2. Streaming Inference Optimization
- **FPS Limiting**: Prevents over-inference, saves resources
- **QPS Tracking**: Sliding window average calculation
- **Concurrency Control**: Per-session independent concurrency limits

### 3. Thermal Protection
```mermaid
flowchart TD
    A[Monitor NPU Temperature] -->|Temp > 85C| B[Trigger Warning]
    A -->|Temp > 80C| C[Auto Throttle]
    C -->|Temp drops| D[Restore Performance]

    subgraph "Performance Statistics"
        E[NPU Utilization] -->|HAL Query| F[Real-time Stats]
        G[CPU Utilization] -->|HAL Query| F
        H[Memory Usage] -->|HAL Query| F
    end
```

### 4. Multi-threading
- **Worker Thread Pool**: N threads for parallel processing
- **Round-Robin Scheduling**: Fair task distribution
- **Condition Variables**: Efficient waiting for new tasks

### 5. Memory Management
- **NPU Context Awareness**: Maximum model cache count control
- **HAL Memory Management**: Automatic output buffer release
- **Reference Counting**: Prevents memory leaks

## Startup Flow

```mermaid
sequenceDiagram
    participant M as Main
    participant C as Config
    participant H as HAL Loader
    participant MM as ModelManager
    participant SM as SessionManager
    participant IS as InferenceScheduler
    participant FR as FdReceiver
    participant EB as EventBus
    participant AS as gRPC Server

    M->>C: Load YAML config
    C->>M: Return Config

    M->>H: Load libaipc_hal.so
    H->>M: Return HAL Ops

    M->>MM: Initialize ModelManager
    M->>SM: Initialize SessionManager
    M->>IS: Initialize InferenceScheduler

    M->>MM: Preload configured models
    MM->>H: HAL register_model
    H->>MM: Return Session

    M->>FR: Start FdReceiver
    FR->>M: Listen on camera socket

    M->>EB: Connect to Event Bus
    EB->>M: Connection successful

    M->>AS: Start gRPC Server
    AS->>M: Listen on Unix Socket

    M->>IS: Start scheduler
    IS->>IS: Start Worker threads

    loop When enabled
        M->>AutoInfer: Start auto inference pipeline
        AutoInfer->>FR: Subscribe to camera stream
    end

    M->>M: Wait for SIGINT/SIGTERM
    M->>AS: Stop service
    AS->>IS: Stop scheduler
    IS->>MM: Release models
    M->>FR: Stop receiver
    M->>EB: Disconnect
```

## Event Bus Integration

### Topic Structure
- **Result Topic**: `inference/{model_id}/{stream_id}`
- **Event ID**: `inf-{frame_seq}-{timestamp_ns}`
- **Message Format**: JSON-formatted PostResult

### Publish Flow
```mermaid
flowchart TD
    A[Inference Complete] --> B[Check auto_publish]
    B -->|Enabled| C[Build JSON Payload]
    B -->|Disabled| D[Skip]

    C --> E[Add Metadata]
    E --> F[Publish to Event Bus]
    F --> G[camera-daemon Subscribe]
    G --> H[Draw AI Overlay]
    H --> I[Video Stream Output]

    subgraph "JSON Format Example"
        J["stream_id: cam1"]
        K["model_id: yolov8n"]
        L["frame_sequence: 123"]
        M["timestamp_ns: 1643123456789"]
        N["detections: [...]"]
    end
```

## GenAI (LLM/VLM) Support

### Initialization Flow
```mermaid
sequenceDiagram
    participant G as GenAI Client
    participant S as ai-runtime
    participant H as HAL GenAI

    G->>S: GenaiCreateSession
    S->>S: force_unregister_all()
    S->>H: Load HEF file
    H->>S: Create GenAI session
    S->>G: Return session_id

    Note over S: Release all models to free NPU space
    Note over S: Wait 6 seconds to ensure HAL cleanup completes
```

### Streaming Generation
```mermaid
flowchart TD
    A[Client sends prompt] --> B[Set generation parameters]
    B --> C[Temperature, Top-P etc.]

    C --> D[Streaming callback mechanism]
    D --> E[Each token generated]
    E --> F[Immediately send to client]
    F --> G[Continue generating next]

    G --> H[Encounter end token]
    H --> I[Send finish signal]

    subgraph "Supported Features"
        J[Text Generation]
        K[Image Understanding]
        L[Multi-turn Dialogue]
        M[LoRA Fine-tuning]
    end
```

## Monitoring and Statistics

### System Statistics Metrics
- **NPU Utilization**: Hardware accelerator usage
- **CPU Utilization**: System CPU usage
- **DSP Utilization**: Digital signal processor load
- **Memory Usage**: Total RAM and used amount
- **Inference Latency**: Queue + inference time
- **FPS Statistics**: Actual inference rate per session

### Log Levels
- DEBUG: Detailed debug information
- INFO: Key operation status
- WARN: Non-fatal warnings
- ERROR: Critical errors

## Troubleshooting

### Common Issues
1. **Model Registration Failed**: Check model path, NPU device permissions
2. **Inference Timeout**: Check queue length, concurrency limits
3. **Zero-Copy Failed**: Check camera-daemon connection
4. **Insufficient Memory**: Adjust model cache count, concurrency limits
5. **Overheating**: Check cooling, reduce load

### Debug Tools
- `nmcli connection show aipc`: Network configuration
- `npu-smi`: Hailo device status
- `journalctl -u ai-runtime`: Service logs
- `perf stat`: Performance profiling

## API Usage Examples

### Python SDK Example
```python
# Register model
model = await runtime.register_model(
    model_id="yolov8n",
    model_path="/opt/aipc/models/yolov8n.hef",
    model_type="detection",
    owner_id="app1"
)

# Create session
session = await runtime.create_session(
    app_id="myapp",
    max_qps=30,
    priority=5
)

# Streaming inference
async for result in runtime.stream_infer(
    session_id=session,
    model_id="yolov8n",
    stream_id="cam1",
    fps_limit=15
):
    detections = result.post_result.detections
    print(f"Found {len(detections)} objects")
```

### Command Line Tool
```bash
# View system statistics
aipc-cli model stats

# Register model (1 arg: model path; optional --id)
aipc-cli model register /data/aipc/models/yolov8n.hef

# List all models
aipc-cli model list

# Get model info
aipc-cli model info <model_id>

# Unregister model
aipc-cli model unregister <model_id>
```

## Version History

### v1.11.0 (Current)
- Added GenAI (LLM/VLM) support
- Optimized DMA-BUF zero-copy performance
- Added temperature monitoring and protection
- Improved scheduler fairness algorithm
- Supported CLIP text encoding

### Future Plans
- Batch inference support
- Model hot-update
- Distributed inference
- ONNX model format support
- More hardware accelerator support

---
