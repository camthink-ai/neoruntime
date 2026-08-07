# Platform API Service

## Overview

`platform-api` is the Web API gateway, providing RESTful HTTP interfaces that proxy backend gRPC services. Built with the Gin framework, it supports WebSocket real-time communication (event streams, video streams, terminal, container logs).

Tech stack: Go + Gin + gRPC client + SQLite (GORM).

## Directory Structure

```
platform/platform-api/
├── auth/                     # Authentication middleware
├── db/                       # Database initialization
├── handlers/                 # HTTP handlers
│   ├── ai.go                # AI Runtime
│   ├── app.go               # App Manager
│   ├── container.go         # Container management
│   ├── device.go            # Device control
│   ├── event.go             # Event bus
│   ├── media.go             # Media configuration
│   ├── stream.go            # Stream configuration
│   ├── h264_stream.go       # H.264 video stream
│   ├── monitor.go           # System monitoring
│   ├── network.go           # Network configuration
│   ├── system.go            # System operations
│   ├── terminal.go          # Web terminal
│   ├── settings.go          # Settings management
│   ├── store.go             # App store
│   ├── dev.go               # Development workbench
│   ├── disk.go              # Storage management
│   ├── file.go              # File management
│   ├── log.go               # Log viewer
│   ├── ssh.go               # SSH configuration
│   ├── time.go              # Time synchronization
│   ├── wizard.go            # Setup wizard
│   └── ...
├── model/                    # Data models
├── repo/                     # Data repositories
├── server/
│   └── main.go              # Entry point
├── storage/                  # Model storage
├── websocket/               # WebSocket handling
└── swagger-ui/              # API documentation UI
```

## API Gateway Architecture

```mermaid
graph TB
    subgraph "Client Layer"
        WC["Web Console<br/>(React)"]
        M["Mobile App"]
        T["Third-party Clients"]
    end

    subgraph "Platform API Gateway"
        subgraph "HTTP Server"
            HS["HTTP Server<br/>(Gin Framework)"]
            RM["Router Manager"]
        end

        subgraph "Middleware Layer"
            AM["Auth Middleware"]
            CM["CORS Middleware"]
            LM["Logging Middleware"]
            MM["Monitoring Middleware"]
        end

        subgraph "Handler Layer"
            MH["HTTP Handlers<br/>(handlers/)"]
            WS["WebSocket Handlers"]
        end

        subgraph "Client Connection Pool"
            GC["gRPC Connection Pool<br/>(Reuse)"]
        end
    end

    subgraph "Backend Service Layer"
        subgraph "5 Core gRPC Services"
            AIR["AI Runtime<br/>Model Management"]
            EB["Event Bus<br/>Event Bus"]
            DC["Device Control<br/>Device Control"]
            AMG["App Manager<br/>Application Management"]
            CC["Camera Control<br/>Media Control"]
        end
    end

    subgraph "Storage Layer"
        DB["SQLite<br/>(GORM)"]
        ES["Event Logs<br/>(Storage)"]
        MS["Model Storage<br/>(CAS)"]
    end

    WC -->|HTTPS| RM
    M -->|HTTPS| RM
    T -->|HTTPS| RM

    RM -->|Request| AM
    AM -->|Auth passed| CM
    CM -->|Process| LM
    LM -->|Route| MH
    MH -->|Proxy| GC
    MH -->|Real-time| WS

    GC -->|gRPC| AIR
    GC -->|gRPC| EB
    GC -->|gRPC| DC
    GC -->|gRPC| AMG
    GC -->|gRPC| CC

    MH -->|Read/Write| DB
    MH -->|Logs| ES
    MH -->|Models| MS

    WS -->|WebSocket| EB
    WS -->|WebSocket| CC
    WS -->|WebSocket| AMG

    style HS fill:#e3f2fd
    style GC fill:#e8f5e9
    style MH fill:#f3e5f5
```

### REST API Call Flow

```mermaid
sequenceDiagram
    participant WC as Web Console
    participant API as Platform API
    participant M as Middleware
    participant H as Handler
    participant GC as gRPC Connection Pool
    participant BS as Backend Service

    Note over WC: Send HTTP request

    WC->>API: POST /api/v1/device/zoom
    API->>M: Auth check
    M->>M: CORS handling
    M->>M: Logging
    M->>H: Route to device handler

    H->>H: Parameter validation
    H->>GC: Get gRPC connection
    GC->>GC: Get from pool (reuse)

    alt Service exists
        GC->>BS: gRPC call
        BS->>BS: Execute device control
        BS-->>GC: Return response
        GC->>GC: Return connection to pool
        GC-->>H: Success response
        H->>WC: JSON response
    else Service not found
        GC->>GC: Connection error
        GC-->>H: Error response
        H->>WC: 503 error
    end

    Note over BS: Backend service processing time ~10-50ms
    Note over API: Gateway processing time ~1-5ms
```

### WebSocket Endpoint Handling Flow

```mermaid
flowchart TD
    A[WebSocket connection request] --> B{Path validation}
    B --> WS_EVENTS["/events/stream"]
    B --> WS_H264["/h264/{stream_id}"]
    B --> WS_LOGS["/containers/{id}/logs/ws"]
    B --> WS_TERMINAL["/terminal/ws"]

    WS_EVENTS --> C[Auth check]
    C --> D[Create EventStream]
    D --> E[Subscribe to EventBus]
    E --> F[Real-time event push]

    WS_H264 --> G[Media authorization]
    G --> H[Start video stream]
    H --> I[Forward H264 data]

    WS_LOGS --> J[Container permission check]
    J --> K[Connect to container log stream]
    K --> L[Real-time log forwarding]

    WS_TERMINAL --> M[SSH authentication]
    M --> N[Create PTY session]
    N --> O[Bidirectional terminal communication]

    style C fill:#e3f2fd
    style G fill:#e8f5e9
    style J fill:#f3e5f5
    style M fill:#fff3e0
```

## Backend Service Connections

| Backend Service | Socket Path | Purpose |
|----------------|-------------|---------|
| AI Runtime | `unix:///run/aipc/ai-runtime.sock` | Model management and inference |
| Event Bus | `unix:///run/aipc/event-bus.sock` | Event subscribe/publish |
| Device Control | `unix:///run/aipc/device-control.sock` | Device control |
| App Manager | `unix:///run/aipc/app-manager.sock` | Application management |
| Camera Control | `unix:///run/aipc/camera-control.sock` | Media/lens |

### gRPC Connection Pool Management

```mermaid
stateDiagram-v2
    [*] --> Initialize

    Initialize --> Create Connection : Establish connection at startup
    Create Connection --> Idle : Connection available
    Idle --> In Use : Acquire connection
    In Use --> Idle : Release connection

    In Use --> Connection Failed : Communication error
    Connection Failed --> Rebuild : Auto-reconnect
    Rebuild --> In Use : Connection restored

    Idle --> Connection Timeout : Unused for too long
    Connection Timeout --> Closed : Release resources
    Closed --> Rebuild : Rebuild when needed
```

## API Endpoint Overview

All endpoints are prefixed with `/api/v1`, some are public (no authentication required).

### Route Organization Structure

```mermaid
graph TD
    A["/api/v1"] --> B[System Management]
    A --> C[Time Management]
    A --> D[AI Models]
    A --> E[Event System]
    A --> F[Device Control]
    A --> G[Application Management]
    A --> H[Container Management]
    A --> I[Media Configuration]
    A --> J[Monitoring Stats]
    A --> K[Storage Management]
    A --> L[File Management]
    A --> M[Network Configuration]
    A --> N[Log System]
    A --> O[Development Tools]

    B --> B1["GET /system/info"]
    B --> B2["GET /system/stats"]
    B --> B3["POST /system/password"]
    B --> B4["POST /system/restart"]

    C --> C1["POST /system/time/sync-from-client"]
    C --> C2["GET /system/time/config"]
    C --> C3["PUT /system/time/ntp"]

    D --> D1["GET /ai/models"]
    D --> D2["POST /ai/models"]
    D --> D3["POST /ai/models/load"]
    D --> D4["POST /ai/models/unload"]

    E --> E1["GET /events/topics"]
    E --> E2["POST /events/publish"]
    E --> E3["GET /events/stream<br/>(WebSocket)"]

    F --> F1["GET /device/status"]
    F --> F2["POST /device/ptz"]
    F --> F3["POST /device/lens/zoom"]
    F --> F4["GET /device/lens/status"]

    G --> G1["GET /apps"]
    G --> G2["POST /apps"]
    G --> G3["POST /apps/{id}/start"]
    G --> G4["GET /apps/{id}/logs"]

    H --> H1["GET /containers"]
    H --> H2["GET /containers/{id}/logs/ws<br/>(WebSocket)"]
    H --> H3["GET /containers/{id}/exec/ws<br/>(WebSocket)"]

    I --> I1["GET /media/config"]
    I --> I2["PUT /media/encoder"]
    I --> I3["PUT /media/ai-overlay"]

    J --> J1["GET /monitor/summary"]
    J --> J2["GET /monitor/cpu"]
    J --> J3["GET /monitor/memory"]

    K --> K1["GET /storage/disks"]
    K --> K2["POST /storage/mount"]
    K --> K3["POST /storage/format"]

    L --> L1["GET /files"]
    L --> L2["POST /files/upload"]
    L --> L3["GET /logs/stream/ws<br/>(WebSocket)"]

    M --> M1["GET /network/config"]
    M --> M2["POST /network/config"]
    M --> M3["GET /network/interfaces"]

    O --> O1["GET /dev/projects"]
    O --> O2["POST /dev/projects/{id}/build"]
    O --> O3["GET /terminal/ws<br/>(WebSocket)"]
```

### System

| Method | Path | Description |
|--------|------|-------------|
| GET | `/system/info` | Platform version and service status |
| GET | `/system/stats` | System statistics |
| GET | `/system/health` | Health check (public) |
| POST | `/system/password` | Change password |
| POST | `/system/restart` | Restart system |
| GET/POST | `/system/ota/*` | OTA upgrade |

### Time Management

| Method | Path | Description |
|--------|------|-------------|
| POST | `/system/time/sync-from-client` | Sync time from client |
| GET/POST | `/system/time/*` | Time/timezone/NTP configuration |

### AI Models

| Method | Path | Description |
|--------|------|-------------|
| GET | `/ai/models` | List loaded models |
| POST | `/ai/models` | Register model |
| POST | `/ai/models/upload` | Upload model file |
| POST | `/ai/models/scan` | Scan available models |
| GET/DELETE | `/ai/models/{id}` | Get/delete model |
| POST | `/ai/models/{id}/load` | Load model |
| POST | `/ai/models/{id}/unload` | Unload model |
| GET | `/ai/stats` | AI statistics |
| GET | `/ai/capabilities` | AI capabilities |

### Events

| Method | Path | Description |
|--------|------|-------------|
| GET | `/events/topics` | List Topics |
| POST | `/events/publish` | Publish event |
| GET (WS) | `/events/stream` | Event stream (WebSocket) |

### Device Control

| Method | Path | Description |
|--------|------|-------------|
| GET | `/device/status` | Device status |
| POST | `/device/light` | White light |
| POST | `/device/ir-led` | IR LED |
| POST | `/device/ir-cut` | IR-Cut |
| POST | `/device/ptz` | PTZ control |
| POST | `/device/zoom` | Zoom |
| POST | `/device/focus` | Focus |
| POST | `/device/autofocus` | Auto focus |
| GET/PUT/POST | `/device/lens/*` | Lens operations |
| POST/GET | `/device/gpio/*` | GPIO |

### Application Management

| Method | Path | Description |
|--------|------|-------------|
| GET | `/apps` | List applications |
| POST | `/apps` | Install application |
| POST | `/apps/wizard` | Wizard install |
| POST | `/apps/upload-image` | Upload image |
| GET | `/apps/{id}` | Application details |
| POST | `/apps/{id}/start` | Start |
| POST | `/apps/{id}/stop` | Stop |
| DELETE | `/apps/{id}` | Uninstall |
| GET | `/apps/{id}/stats` | Statistics |
| GET | `/apps/{id}/logs` | Logs |

### Container Management

| Method | Path | Description |
|--------|------|-------------|
| GET | `/containers` | List containers |
| GET | `/containers/{id}` | Container details |
| GET | `/containers/{id}/stats` | Container statistics |
| GET (WS) | `/containers/{id}/logs/ws` | Log stream |
| GET (WS) | `/containers/{id}/exec/ws` | Container terminal |
| POST | `/containers/{id}/start` | Start |
| POST | `/containers/{id}/stop` | Stop |
| DELETE | `/containers/{id}` | Delete |

### Media Configuration

| Method | Path | Description |
|--------|------|-------------|
| GET | `/media/status` | Stream status |
| GET/POST | `/media/config` | Media configuration |
| PUT | `/media/encoder` | Hot update encoding parameters |
| PUT | `/media/rtsp` | RTSP toggle |
| PUT | `/media/ai-overlay` | AI overlay |
| PUT | `/media/osd` | OSD configuration |
| GET | `/media/profile`, `/media/profiles` | Profile info/list |
| POST | `/media/profile/switch`, `/media/profile/backup` | Switch/backup profile |
| POST | `/media/streams/{name}/enable` | Enable stream |
| DELETE | `/media/streams/{name}/disable` | Disable stream |

### H.264 Video Stream

| Method | Path | Description |
|--------|------|-------------|
| GET (WS) | `/h264/{stream_id}` | H.264 WebSocket stream |

### System Monitoring

| Method | Path | Description |
|--------|------|-------------|
| GET | `/monitor/summary` | Resource overview |
| GET | `/monitor/cpu` | CPU usage |
| GET | `/monitor/memory` | Memory usage |
| GET | `/monitor/disk` | Disk usage |
| GET | `/monitor/network` | Network usage |
| GET (SSE) | `/monitor/gyro/attitude` | Gyroscope tilt stream (pitch/roll) — see [`gyro-attitude-sse.md`](../references/gyro-attitude-sse.md) |

### Storage Management

| Method | Path | Description |
|--------|------|-------------|
| GET | `/storage/disks` | Disk list |
| POST | `/storage/mount` | Mount |
| POST | `/storage/unmount` | Unmount |
| POST | `/storage/format` | Format |

### File Management

| Method | Path | Description |
|--------|------|-------------|
| GET | `/files` | List files |
| GET/POST | `/files/content` | Read/write files |
| POST | `/files/upload` | Upload file |
| GET | `/files/download` | Download file |
| DELETE | `/files` | Delete file |

### Terminal

| Method | Path | Description |
|--------|------|-------------|
| GET (WS) | `/terminal/ws` | Web terminal |

### Network Configuration

| Method | Path | Description |
|--------|------|-------------|
| GET | `/network/config` | Network configuration |
| POST | `/network/config` | Update configuration |
| GET | `/network/interfaces` | Network interface list |

### Logs

| Method | Path | Description |
|--------|------|-------------|
| GET | `/logs/services` | Service list |
| GET | `/logs/content` | Log content |
| GET (WS) | `/logs/stream/ws` | Log stream |

### WebSocket Endpoints

| Path | Purpose |
|------|---------|
| `/api/v1/events/stream?token=` | Event stream |
| `/api/v1/h264/{stream_id}` | H.264 video stream |
| `/api/v1/containers/{id}/logs/ws` | Container log stream |
| `/api/v1/containers/{id}/exec/ws` | Container terminal |
| `/api/v1/terminal/ws` | Web terminal |
| `/api/v1/logs/stream/ws` | Service log stream |

## Authentication

- Mandatory Bearer Token authentication (`auth.enabled: true`; required except for public endpoints)
- Header: `Authorization: Bearer <token>` or `X-API-Key: <token>`
- WebSocket: `?token=<token>` query parameter
- Public endpoints: `/api/login`, `/api/v1/system/health`, `/api/v1/auth/public-key`,
  `/api/v1/system/ota/status`, `/api/v1/system/os-upgrade/status`

### Authentication Middleware Flow

```mermaid
sequenceDiagram
    participant C as Client
    participant API as Platform API
    participant Auth as Auth Middleware
    participant DB as User Database

    C->>API: HTTP Request with Authorization
    API->>Auth: Pass through middleware
    Auth->>Auth: Extract token from header/query
    alt Token present
        Auth->>DB: Validate token
        DB-->>Auth: Token valid/invalid
        Auth->>Auth: Check expiration
        alt Token valid
            Auth->>API: Allow request
            API->>API: Process request
            API-->>C: Response
        else Token invalid
            Auth->>C: 401 Unauthorized
        end
    else No token
        Auth->>Auth: Check public endpoint
        alt Public endpoint
            Auth->>API: Allow request
        else Protected endpoint
            Auth->>C: 401 Unauthorized
        end
    end
```

## Configuration

```yaml
service:
  name: platform-api
  http_addr: "127.0.0.1:8080"
  log_level: info

services:
  ai_runtime: "unix:///run/aipc/ai-runtime.sock"
  event_bus: "unix:///run/aipc/event-bus.sock"
  device_control: "unix:///run/aipc/device-control.sock"
  app_manager: "unix:///run/aipc/app-manager.sock"
  camera_control: "unix:///run/aipc/camera-control.sock"

web:
  static_path: "/data/aipc/web"
  enable_cors: true

auth:
  enabled: true
  token_key: ""
  username: "admin"
  password: "password"

stream:
  encoded_pub_dir: "/run/aipc/encoded"

storage:
  root_path: "/data/aipc"
  model_blob_path: "/data/aipc/models/blobs"

# Gyro / attitude SSE source (LSM6DSR IMU via Linux IIO sysfs).
# Drives GET /api/v1/monitor/gyro/attitude. Disable on hosts without the IMU;
# a missing sensor degrades gracefully (status=offline, no crash).
gyro:
  enabled: true
  poll_rate_hz: 26        # sensor read cadence; must stay <= odr_hz
  odr_hz: 26              # device output data rate (0 = code default)
  fusion_alpha: 0.05      # complementary filter coefficient in (0,1)
  iio_base: "/sys/bus/iio/devices"
  # accel_path / gyro_path / calibration_path / use_calibration_* overrides optional
  mount_matrix: [1, 0, 0,  0, 1, 0,  0, 0, 1]   # row-major body -> world
  calibrate_bias: true    # one-shot static gyro-bias calibration at boot
  calibration_ms: 2000    # averaging window for bias calibration
```

## Response Format

```json
{
  "code": 0,
  "message": "Success",
  "data": { ... }
}
```

## Middleware Chain

```mermaid
flowchart LR
    A[Request arrives] --> B[CORS handling]
    B --> C[Logging]
    C --> D[Auth check]
    D --> E[Route matching]
    E --> F[Parameter validation]
    F --> G[Permission check]
    G --> H[Business logic]
    H --> I[Response wrapping]
    I --> J[Return response]

    style B fill:#e3f2fd
    style D fill:#f3e5f5
    style G fill:#e8f5e9
    style H fill:#fff3e0
```

## API Documentation

Swagger UI is accessible at `/swagger/`.