# AIPC Platform Architecture Documentation

## Table of Contents

- [System Overview](#system-overview)
- [Architecture Layers](#architecture-layers)
- [Core Components](#core-components)
- [Data Flow](#data-flow)
- [Service Interaction Diagram](#service-interaction-diagram)
- [Deployment Architecture](#deployment-architecture)
- [Security Design](#security-design)
- [Extensibility](#extensibility)
- [Performance Considerations](#performance-considerations)

## System Overview

AIPC is a generic edge AI computing platform designed for smart IP cameras, industrial cameras, and edge boxes.

### Core Objectives

1. **Cross-SoC Portability**: Supports multiple hardware platforms through the HAL abstraction layer
2. **Open Architecture**: Supports third-party models and application containers
3. **Secure Isolation**: Container-based sandboxing mechanism
4. **Unified Inference**: Centralized AI compute management
5. **Peripheral Control**: Unified hardware peripheral management through MCU

### Supported SoCs

- **Hailo-15** (launch platform; `hal_v2/platforms/hailo15/`)
- **Stub** (host dev/test backend; `hal_v2/platforms/stub/`)
- Others (via HAL adaptation)

## Architecture Layers

### Layered Architecture Diagram

```mermaid
graph TB
    subgraph "Application Container Layer"
        A1[Business Service Container<br/>Business Services<br/>Python/Go/C++]
        A2[Model Service Container<br/>Model Services<br/>Inference Pipeline]
    end

    subgraph "Platform Services Layer"
        subgraph "Go Microservices"
            B2[event-bus<br/>Event Bus]
            B3[app-manager<br/>Application Lifecycle Management]
            B4[device-control<br/>Device Control]
            B5[device-discovery<br/>Device Discovery]
            B6[platform-api<br/>HTTP API Gateway]
        end

        subgraph "C++ Services"
            B1[ai-runtime<br/>AI Inference Service]
            B7[camera-daemon<br/>Video Processing Daemon]
        end
    end

    subgraph "Hardware Abstraction Layer (HAL)"
        subgraph "HAL Interfaces"
            C1[hal_video.h<br/>Video Capture]
            C2[hal_inference.h<br/>AI Inference]
            C3[hal_codec.h<br/>Video Encoding/Decoding]
            C4[hal_io.h<br/>Peripheral Control]
            C5[hal_buffer.h<br/>Frame Buffer]
        end

        subgraph "HAL Implementations"
            D1[hailo15/<br/>Hailo-15 Implementation]
            D2[stub/<br/>Test Stub Implementation]
        end
    end

    subgraph "Hardware Layer"
        E1[SoC Chip]
        E2[Sensor/ISP]
        E3[NPU/AI Accelerator]
        E4[MCU]
    end

    A1 --> B6
    A2 --> B6
    B1 --> B7
    B2 --> B7
    B3 --> B7
    B4 --> B7
    B5 --> B7
    B6 --> B7
    B7 --> C1
    B7 --> C2
    B7 --> C3
    B7 --> C4
    C1 --> D1
    C1 --> D2
    C1 --> D3
    C1 --> D4
    C2 --> D1
    C2 --> D2
    C2 --> D3
    C2 --> D4
    C3 --> D1
    C3 --> D2
    C3 --> D3
    C3 --> D4
    C4 --> D1
    C4 --> D2
    C4 --> D3
    C4 --> D4
    D1 --> E1
    D1 --> E2
    D1 --> E3
    D1 --> E4
    D2 --> E1
    D2 --> E2
    D2 --> E3
    D2 --> E4
    D3 --> E1
    D3 --> E2
    D3 --> E3
    D3 --> E4
    D4 --> E1
    D4 --> E2
    D4 --> E3
    D4 --> E4

    style A1 fill:#e1f5fe,stroke:#01579b
    style A2 fill:#e1f5fe,stroke:#01579b
    style B1 fill:#e8f5e9,stroke:#1b5e20
    style B2 fill:#e8f5e9,stroke:#1b5e20
    style B3 fill:#e8f5e9,stroke:#1b5e20
    style B4 fill:#e8f5e9,stroke:#1b5e20
    style B5 fill:#e8f5e9,stroke:#1b5e20
    style B6 fill:#e8f5e9,stroke:#1b5e20
    style B7 fill:#fff3e0,stroke:#e65100
    style C1 fill:#fce4ec,stroke:#880e4f
    style C2 fill:#fce4ec,stroke:#880e4f
    style C3 fill:#fce4ec,stroke:#880e4f
    style C4 fill:#fce4ec,stroke:#880e4f
    style C5 fill:#fce4ec,stroke:#880e4f
    style D1 fill:#f3e5f5,stroke:#4a148c
    style D2 fill:#f3e5f5,stroke:#4a148c
    style D3 fill:#f3e5f5,stroke:#4a148c
    style D4 fill:#f3e5f5,stroke:#4a148c
    style E1 fill:#efebe9,stroke:#3e2723
    style E2 fill:#efebe9,stroke:#3e2723
    style E3 fill:#efebe9,stroke:#3e2723
    style E4 fill:#efebe9,stroke:#3e2723
```

### Layer Responsibilities

#### 1. Application Container Layer

- Runs user-defined applications
- Divided into two categories: business services and model services
- Accesses platform capabilities through SDK
- Fully sandboxed and isolated

#### 2. Platform Services Layer

- **ai-runtime**: AI inference service management, model loading and scheduling, NPU compute quota control
- **camera-daemon**: C++ media pipeline management, outputs encoded streams + FD-based frames, RTSP server
- **event-bus**: Local event bus, supports wildcard patterns, low latency (< 1ms)
- **app-manager**: Container lifecycle management, manifest parsing, resource quotas
- **device-control**: MCU/peripheral control, light, PTZ, GPIO management
- **device-discovery**: Network device discovery and management (CT-Disc protocol)
- **platform-api**: Web API Gateway, HTTP to gRPC forwarding

#### 3. HAL Layer

Unified hardware interfaces with dynamic loading of platform-specific implementations:
- `hal_video.h`: Camera/ISP management
- `hal_inference.h`: NPU/AI accelerator interface
- `hal_codec.h`: Hardware encoding/decoding (H.264/H.265)
- `hal_io.h`: MCU/GPIO/peripheral control
- `hal_buffer.h`: Unified frame buffer management with DMA-BUF zero-copy support

#### 4. Hardware Layer

- SoC chips and drivers
- Sensor, ISP, NPU
- MCU (controls peripherals)

## Core Components

### Camera Daemon

**Responsibilities:**
- Manages media pipeline (Hailo Media Library)
- Outputs encoded streams (H.264/H.265)
- Outputs raw frames via FD-based protocol (for AI use)
- Dynamically adjusts ISP parameters
- Supports multiple video sources

**Tech Stack:** C++ (Hailo Media Library / HAL)

### AI Runtime

**Responsibilities:**
- Model loading and management
- Inference request scheduling
- Compute quota control
- Interacts with HAL.ML
- Supports session management

**Tech Stack:** C++17 (HAL binding)

**Key Features:**
- Unary Infer
- Stream Infer
- Multi-model concurrency
- QPS/priority control
- Compute quota management

### Event Bus

**Responsibilities:**
- Local Pub/Sub message bus
- AI result distribution
- Application event reporting
- System event notification

**Tech Stack:** Go

**Features:**
- Topic wildcard support (`*` and `>`)
- Persistence (optional)
- Low latency (< 1ms)
- Batch publishing

### App Manager

**Responsibilities:**
- Container lifecycle management
- Manifest parsing and permission control
- Resource quotas (CPU/memory/AI)
- Security sandbox configuration
- Container image management

**Tech Stack:** Go + containerd

### Device Control

**Responsibilities:**
- MCU communication (UART protocol)
- Light control (white light/IR/IR-Cut)
- PTZ control
- Zoom and focus
- GPIO control
- Device status monitoring

**Tech Stack:** Go + HAL.IO

## Data Flow

### Video Stream Processing

```mermaid
graph LR
    A[Sensor] --> B[ISP]
    B --> C[GStreamer Pipeline]
    C --> D[camera-daemon]
    D --> E1[Raw SHM<br/>For AI Use]
    D --> E2[Encoded H.264/H.265<br/>For Streaming]
    D --> E3[DMA-BUF FD<br/>To ai-runtime]
    D --> E4[RTSP<br/>Remote Access]

    style A fill:#e3f2fd,stroke:#1565c0
    style B fill:#e3f2fd,stroke:#1565c0
    style C fill:#e3f2fd,stroke:#1565c0
    style D fill:#e8f5e9,stroke:#2e7d32
    style E1 fill:#fff3e0,stroke:#ef6c00
    style E2 fill:#fff3e0,stroke:#ef6c00
    style E3 fill:#fff3e0,stroke:#ef6c00
    style E4 fill:#fff3e0,stroke:#ef6c00
```

### AI Inference Flow

```mermaid
graph TB
    A[Raw SHM<br/>camera-daemon] --> B[ai-runtime]
    B --> C[HAL.ML]
    C --> D[NPU]
    D --> E[Inference Results]
    E --> F[event-bus]
    F --> G1[Business Service Container]
    F --> G2[Model Service Container]
    F --> G3[Web Console]

    subgraph "Zero-Copy Path"
        A -- DMA-BUF --> B
    end

    subgraph "Result Distribution"
        F -- Pub/Sub --> G1
        F -- Pub/Sub --> G2
        F -- HTTP API --> G3
    end

    style A fill:#e1f5fe,stroke:#01579b
    style B fill:#e8f5e9,stroke:#1b5e20
    style C fill:#fce4ec,stroke:#880e4f
    style D fill:#efebe9,stroke:#3e2723
    style E fill:#fff3e0,stroke:#e65100
    style F fill:#f3e5f5,stroke:#4a148c
```

### Event Flow

```mermaid
graph LR
    A[Model Service Container] --> B[Inference Complete Event]
    B --> C[event-bus]
    C --> D1[Business Service Container]
    C --> D2[Web Console]
    C --> D3[External Platform]

    E[Application Container] --> F[Custom Event]
    F --> C
    C --> D2
    C --> D3

    style A fill:#e1f5fe,stroke:#01579b
    style B fill:#fff3e0,stroke:#ef6c00
    style C fill:#f3e5f5,stroke:#4a148c
    style D1 fill:#e1f5fe,stroke:#01579b
    style D2 fill:#e8f5e9,stroke:#1b5e20
    style D3 fill:#e8f5e9,stroke:#1b5e20
    style E fill:#e1f5fe,stroke:#01579b
    style F fill:#fff3e0,stroke:#ef6c00
```

### Peripheral Control Flow

```mermaid
graph TB
    A[Application Container] --> B[device-control gRPC]
    B --> C[HAL.IO]
    C --> D[MCU]
    D --> E1[Light Control]
    D --> E2[PTZ Control]
    D --> E3[GPIO Control]
    D --> E4[Lens Control]

    E1 --> F
    E2 --> F
    E3 --> F
    E4 --> F
    F --> G[Status Feedback]
    G --> D
    D --> C
    C --> B
    B --> A

    style A fill:#e1f5fe,stroke:#01579b
    style B fill:#e8f5e9,stroke:#1b5e20
    style C fill:#fce4ec,stroke:#880e4f
    style D fill:#efebe9,stroke:#3e2723
    style E1 fill:#fff3e0,stroke:#ef6c00
    style E2 fill:#fff3e0,stroke:#ef6c00
    style E3 fill:#fff3e0,stroke:#ef6c00
    style E4 fill:#fff3e0,stroke:#ef6c00
```

## Service Interaction Diagram

### Microservice Communication Architecture

```mermaid
graph TB
    subgraph "API Gateway"
        API[platform-api<br/>HTTP:8080]
    end

    subgraph "Internal Services"
        subgraph "Go Services"
            ER[ai-runtime<br/>Unix Socket]
            EB[event-bus<br/>Unix Socket + TCP]
            AM[app-manager<br/>Unix Socket]
            DC[device-control<br/>Unix Socket]
            DD[device-discovery<br/>Unix Socket]
        end

        subgraph "C++ Service"
            CD[camera-daemon]
        end
    end

    subgraph "External Clients"
        WC[Web Console]
        SDK[Python SDK]
        CLI[aipc CLI]
    end

    %% External Access
    WC -- REST API --> API
    SDK -- gRPC --> API
    CLI -- gRPC --> API

    %% API Gateway Forwarding
    API -- gRPC --> ER
    API -- gRPC --> EB
    API -- gRPC --> AM
    API -- gRPC --> DC
    API -- gRPC --> DD

    %% Internal Service Communication
    ER -- DMA-BUF/SHM --> CD
    CD -- Video Stream --> ER
    ER -- Inference Results --> EB
    AM -- App Lifecycle --> DC
    AM -- Container Events --> EB
    DC -- Device Events --> EB
    DD -- Discovery Events --> EB

    %% Event Bus Connections
    EB -- Pub/Sub --> WC
    EB -- Pub/Sub --> SDK

    style API fill:#ffecb3,stroke:#ff8f00
    style ER fill:#c8e6c9,stroke:#388e3c
    style EB fill:#c8e6c9,stroke:#388e3c
    style AM fill:#c8e6c9,stroke:#388e3c
    style DC fill:#c8e6c9,stroke:#388e3c
    style DD fill:#c8e6c9,stroke:#388e3c
    style CD fill:#ffccbc,stroke:#d84315
    style WC fill:#bbdefb,stroke:#1565c0
    style SDK fill:#bbdefb,stroke:#1565c0
    style CLI fill:#bbdefb,stroke:#1565c0
```

## Deployment Architecture

### SystemD Service Dependencies

```mermaid
graph TD
    subgraph "SystemD Service Hierarchy"
        Network[Network Service<br/>NetworkManager]
        System[System Service<br/>systemd-udevd]

        %% Platform Services
        subgraph "Core Services"
            Bus[event-bus.service<br/>Event Bus]
            API[platform-api.service<br/>API Gateway]
            AI[ai-runtime.service<br/>AI Runtime]
            Camera[camera-daemon.service<br/>Video Daemon]
        end

        %% Management Services
        subgraph "Management Services"
            App[app-manager.service<br/>App Manager]
            Control[device-control.service<br/>Device Control]
            Discovery[device-discovery.service<br/>Device Discovery]
        end

        %% Container Services
        subgraph "Container Services"
            Container1[app1.service<br/>Application 1]
            Container2[app2.service<br/>Application 2]
            ContainerN[appN.service<br/>Application N]
        end
    end

    %% Base Services
    System --> Bus
    System --> API

    %% Core Service Startup Order
    Bus --> API
    API --> AI
    API --> Camera
    Camera --> App

    %% Management Services
    App --> Control
    App --> Discovery

    %% Container Services
    App --> Container1
    App --> Container2
    App --> ContainerN

    style Network fill:#e1f5fe,stroke:#01579b
    style System fill:#e1f5fe,stroke:#01579b
    style Bus fill:#e8f5e9,stroke:#1b5e20
    style API fill:#e8f5e9,stroke:#1b5e20
    style AI fill:#e8f5e9,stroke:#1b5e20
    style Camera fill:#e8f5e9,stroke:#1b5e20
    style App fill:#fff3e0,stroke:#ef6c00
    style Control fill:#fff3e0,stroke:#ef6c00
    style Discovery fill:#fff3e0,stroke:#ef6c00
    style Container1 fill:#fce4ec,stroke:#880e4f
    style Container2 fill:#fce4ec,stroke:#880e4f
    style ContainerN fill:#fce4ec,stroke:#880e4f
```

## Security Design

### Container Isolation Architecture

```mermaid
graph TB
    subgraph "Host System"
        HostKernel[Linux Kernel]

        subgraph "Namespace Isolation"
            NamespacePID[PID Namespace]
            NamespaceNET[NET Namespace]
            NamespaceIPC[IPC Namespace]
            NamespaceMNT[MNT Namespace]
            NamespaceUTS[UTS Namespace]
        end

        subgraph "Cgroup Limits"
            CGroupCPU[CPU Quota]
            CGroupMem[Memory Limit]
            CGroupIO[IO Throttle]
        end

        subgraph "Security Modules"
            Seccomp[Seccomp Filtering]
            Capabilities[Capabilities Trimming]
        end
    end

    subgraph "Container Runtime"
        Containerd[containerd]
        subgraph "Sandboxed Containers"
            Container1[Application Container 1]
            Container2[Model Container 2]
            ContainerN[Container N]
        end
    end

    subgraph "Platform Services"
        Platform[Go Microservices<br/>No Container]
    end

    %% Isolation Mechanisms
    HostKernel --> NamespacePID
    HostKernel --> NamespaceNET
    HostKernel --> NamespaceIPC
    HostKernel --> NamespaceMNT
    HostKernel --> NamespaceUTS
    HostKernel --> CGroupCPU
    HostKernel --> CGroupMem
    HostKernel --> CGroupIO
    HostKernel --> Seccomp
    HostKernel --> Capabilities

    %% Runtime
    NamespacePID --> Containerd
    NamespaceNET --> Containerd
    NamespaceIPC --> Containerd
    NamespaceMNT --> Containerd
    NamespaceUTS --> Containerd
    Containerd --> Container1
    Containerd --> Container2
    Containerd --> ContainerN
    Seccomp --> Platform
    Capabilities --> Platform

    style HostKernel fill:#37474f,stroke:#263238
    style Containerd fill:#607d8b,stroke:#455a64
    style Container1 fill:#e1f5fe,stroke:#01579b
    style Container2 fill:#e1f5fe,stroke:#01579b
    style ContainerN fill:#e1f5fe,stroke:#01579b
    style Platform fill:#e8f5e9,stroke:#1b5e20
```

### Permission Model

```mermaid
graph TD
    subgraph "Application Manifest Permission Declarations"
        AppManifest[app.yaml<br/>Permission Configuration]

        subgraph "Video Permissions"
            PermVideo["/video/cam0_main.raw"]
        end

        subgraph "AI Inference Permissions"
            PermInference["/inference/ Model List QPS Limit"]
        end

        subgraph "Event Permissions"
            PermEvent["/publish/topic /subscribe/topic"]
        end

        subgraph "Device Permissions"
            PermDevice["/light /ptz /gpio"]
        end

        subgraph "Network Permissions"
            PermNetwork["/outbound/hosts"]
        end
    end

    subgraph "Security Sandbox"
        subgraph "Container Isolation"
            Container[containerd<br/>namespace]
        end

        subgraph "System Restrictions"
            Seccomp[Seccomp<br/>Filter Dangerous Syscalls]
            Caps[Capabilities<br/>Keep Only Essential]
            Rootfs[Read-only rootfs<br/>no_new_privileges]
        end

        subgraph "Resource Limits"
            Cpu[CPU Quota Limit]
            Mem[Memory Limit]
            Pid[Process Count Limit]
        end
    end

    subgraph "Runtime Checks"
        Runtime[Permission Verification<br/>Runtime Monitoring]
    end

    AppManifest --> PermVideo
    AppManifest --> PermInference
    AppManifest --> PermEvent
    AppManifest --> PermDevice
    AppManifest --> PermNetwork
    PermVideo --> Container
    PermInference --> Container
    PermEvent --> Container
    PermDevice --> Container
    PermNetwork --> Container
    Container --> Seccomp
    Container --> Caps
    Container --> Rootfs
    Container --> Cpu
    Container --> Mem
    Container --> Pid
    Seccomp --> Runtime
    Caps --> Runtime
    Rootfs --> Runtime
    Cpu --> Runtime
    Mem --> Runtime
    Pid --> Runtime

    style AppManifest fill:#fff3e0,stroke:#ef6c00
    style Container fill:#e8f5e9,stroke:#1b5e20
    style Seccomp fill:#fce4ec,stroke:#880e4f
    style Caps fill:#fce4ec,stroke:#880e4f
    style Rootfs fill:#fce4ec,stroke:#880e4f
    style Cpu fill:#e1f5fe,stroke:#01579b
    style Mem fill:#e1f5fe,stroke:#01579b
    style Pid fill:#e1f5fe,stroke:#01579b
    style Runtime fill:#f3e5f5,stroke:#4a148c
```

## Extensibility

### Adding a New SoC Flow

```mermaid
graph TB
    A[New SoC Support] --> B[Design Implementation]
    B --> C[Implement HAL Interfaces]
    C --> D[Compile to .so]
    D --> E[Configure Load Path]
    E --> F[Test and Verify]

    subgraph "HAL Interface Implementation"
        C1[hal_video_ops<br/>Video Capture]
        C2[hal_inference_ops<br/>AI Acceleration]
        C3[hal_codec_ops<br/>Encoding/Decoding]
        C4[hal_io_ops<br/>Peripheral Control]
    end

    subgraph "Build System Integration"
        D1[Makefile Configuration]
        D2[CMake Cross-Compilation]
        D3[Toolchain Files]
    end

    subgraph "Test Verification"
        E1[Unit Tests]
        E2[Integration Tests]
        E3[Performance Benchmarks]
    end

    C --> C1
    C --> C2
    C --> C3
    C --> C4
    D --> D1
    D --> D2
    D --> D3
    E --> E1
    E --> E2
    E --> E3

    style A fill:#e8f5e9,stroke:#1b5e20
    style B fill:#e8f5e9,stroke:#1b5e20
    style C fill:#e8f5e9,stroke:#1b5e20
    style C1 fill:#fce4ec,stroke:#880e4f
    style C2 fill:#fce4ec,stroke:#880e4f
    style C3 fill:#fce4ec,stroke:#880e4f
    style C4 fill:#fce4ec,stroke:#880e4f
    style D fill:#e8f5e9,stroke:#1b5e20
    style D1 fill:#fff3e0,stroke:#ef6c00
    style D2 fill:#fff3e0,stroke:#ef6c00
    style D3 fill:#fff3e0,stroke:#ef6c00
    style E fill:#e8f5e9,stroke:#1b5e20
    style E1 fill:#e1f5fe,stroke:#01579b
    style E2 fill:#e1f5fe,stroke:#01579b
    style E3 fill:#e1f5fe,stroke:#01579b
```

## Performance Considerations

### Zero-Copy Optimization Architecture

```mermaid
graph TB
    subgraph "Video Pipeline"
        subgraph "Sensor Capture"
            Sensor[Sensor<br/>Physical Camera]
        end

        subgraph "Zero-Copy Transfer"
            ISP[ISP<br/>Image Signal Processing]
            DMABUF1[DMA-BUF<br/>Zero-Copy Sharing]
            HAL_V[HAL.Video<br/>Video Abstraction]
        end

        subgraph "Processing Units"
            Camera[Camera-Daemon<br/>GStreamer]
            AI[AI Runtime<br/>Inference Service]
            Codec[HAL.Codec<br/>Encoder]
        end

        subgraph "Output"
            SHM[SHM Memory<br/>Application Container]
            RTSP[RTSP<br/>Streaming]
        end
    end

    %% Zero-Copy Path
    Sensor --> ISP
    ISP --> DMABUF1
    DMABUF1 --> HAL_V
    HAL_V --> Camera
    Camera --> DMABUF1 --> AI
    AI --> DMABUF1 --> Codec

    %% Normal Copy Path
    Camera --> SHM
    Codec --> RTSP

    style Sensor fill:#37474f,stroke:#263238
    style ISP fill:#455a64,stroke:#37474f
    style DMABUF1 fill:#ffeb3b,stroke:#f57f17,bold
    style HAL_V fill:#64b5f6,stroke:#1976d2
    style Camera fill:#81c784,stroke:#388e3c
    style AI fill:#81c784,stroke:#388e3c
    style Codec fill:#81c784,stroke:#388e3c
    style SHM fill:#ffb74d,stroke:#f57c00
    style RTSP fill:#ffb74d,stroke:#f57c00
```

### Concurrent Processing Architecture

```mermaid
graph TD
    subgraph "Concurrent Input Sources"
        Video[Video Stream Input<br/>Multiple Channels]
        Inference[Inference Requests<br/>Multiple Models]
        Events[Event Messages<br/>Multiple Topics]
    end

    subgraph "Service Layer Concurrency"
        subgraph "AI Runtime"
            AI_Q1[Model 1 Queue]
            AI_Q2[Model 2 Queue]
            AI_QN[Model N Queue]
        end

        subgraph "Event Bus"
            EB_Q1[Topic 1 Queue]
            EB_Q2[Topic 2 Queue]
            EB_QN[Topic N Queue]
        end

        subgraph "App Manager"
            AM_Q[Container Management<br/>Task Queue]
        end
    end

    subgraph "Output Distribution"
        Sub1[Subscriber 1]
        Sub2[Subscriber 2]
        SubN[Subscriber N]
    end

    %% Concurrent Processing Flow
    Video --> AI_Q1
    Video --> AI_Q2
    Video --> AI_QN
    Inference --> AI_Q1
    Inference --> AI_Q2
    Inference --> AI_QN
    Events --> EB_Q1
    Events --> EB_Q2
    Events --> EB_QN

    %% Schedulers
    subgraph "Schedulers"
        Sched_AI[AI Scheduler<br/>QPS Control]
        Sched_Event[Event Scheduler<br/>Topic Routing]
        Sched_App[App Scheduler<br/>Priority Queue]
    end

    AI_Q1 --> Sched_AI
    AI_Q2 --> Sched_AI
    AI_QN --> Sched_AI
    EB_Q1 --> Sched_Event
    EB_Q2 --> Sched_Event
    EB_QN --> Sched_Event
    AM_Q --> Sched_App

    %% Result Distribution
    Sched_AI --> Results[Inference Results]
    Sched_Event --> Events2[Event Distribution]
    Sched_App --> Actions[Application Actions]

    Results --> Sub1
    Results --> Sub2
    Results --> SubN
    Events2 --> Sub1
    Events2 --> Sub2
    Events2 --> SubN

    style Video fill:#e1f5fe,stroke:#01579b
    style Inference fill:#e1f5fe,stroke:#01579b
    style Events fill:#e1f5fe,stroke:#01579b
    style AI_Q1 fill:#e8f5e9,stroke:#1b5e20
    style AI_Q2 fill:#e8f5e9,stroke:#1b5e20
    style AI_QN fill:#e8f5e9,stroke:#1b5e20
    style EB_Q1 fill:#e8f5e9,stroke:#1b5e20
    style EB_Q2 fill:#e8f5e9,stroke:#1b5e20
    style EB_QN fill:#e8f5e9,stroke:#1b5e20
    style AM_Q fill:#e8f5e9,stroke:#1b5e20
    style Sched_AI fill:#fff3e0,stroke:#ef6c00
    style Sched_Event fill:#fff3e0,stroke:#ef6c00
    style Sched_App fill:#fff3e0,stroke:#ef6c00
    style Sub1 fill:#fce4ec,stroke:#880e4f
    style Sub2 fill:#fce4ec,stroke:#880e4f
    style SubN fill:#fce4ec,stroke:#880e4f
```

## Reference Documentation

- [HAL v2 API Reference](../references/hal-v2-api-reference.md)
- [REST API Reference (Swagger)](../api/swagger.yaml)
- [MCU Communication Protocol](../mcu_protocol/README.md)
