# Media Streaming Architecture

## Overview

NE503 implements a complete low-latency video streaming pipeline, from camera hardware capture to Web frontend playback, supporting both RTSP and WebSocket protocols. The system uses a dual-thread design, Unix Domain Socket communication, zero-copy optimization, and other high-performance techniques to ensure real-time requirements are met.

### Core Features

- **Protocol Support**: RTSP 1.0 (RFC 2326) + RTP/AVP/TCP interleaved transport
- **Encoding/Decoding**: H.264 (RFC 6184) and H.265 (RFC 7798) FU-A fragmentation
- **Concurrency**: Multiple clients can consume the same stream simultaneously
- **Low Latency**: Full pipeline optimized for < 100ms end-to-end
- **Hot Update**: Dynamic encoding parameter adjustment supported
- **Fault Tolerance**: Auto-reconnect, error recovery, stream interruption recovery

## Data Flow Architecture

```mermaid
sequenceDiagram
    participant Camera as Camera Hardware
    participant HAL as HAL.Video
    participant Encoder as HAL.Codec
    participant Publisher as EncodedPublisher
    participant RTSP as RTSP Server
    participant API as Platform-API
    participant Frontend as Web Frontend
    participant Player as Player

    Camera->>HAL: Capture YUV data
    HAL->>Encoder: YUV -> H.264/H.265 encoding
    Encoder->>Publisher: Callback on_packet() (zero-copy)
    Publisher->>Publisher: Dual-thread dispatch (encode callback + socket)
    Publisher->>RTSP: Unix Domain Socket (.sock)
    RTSP->>RTSP: SDP generation (SPS/PPS)
    RTSP->>Frontend: RTSP over TCP (8554)

    Publisher->>API: WebSocket proxy
    API->>Frontend: H.264 over WebSocket (/api/v1/h264/:stream)
    Frontend->>Player: WebCodecs/MSE decoding
    Player->>Player: Canvas rendering

    Note over Publisher: V1 22-byte / V2 30-byte protocol auto-detection
    Note over API: Annex-B to AVCC format conversion
    Note over Player: Exponential backoff reconnection (1s -> 10s)
```

## Component Deep Dive

### 1. RTSP Server (`platform/camera-daemon/src/rtsp_server.cpp`)

#### Architecture Features
- **Protocol Implementation**: RTSP 1.0 + RTP/AVP/TCP (RFC 7826 interleaved transport)
- **Supported Formats**: H.264 (RFC 6184) and H.265 (RFC 7798)
- **Fragmentation Strategy**: FU-A for large NAL units (> 1400 bytes)
- **Multi-client**: Supports 8 concurrent RTSP clients
- **Port**: 8554 (configurable)

#### RTSP State Machine

```mermaid
stateDiagram-v2
    [*] --> INIT: New connection established
    INIT --> READY: DESCRIBE request
    READY --> PLAYING: SETUP + PLAY request
    PLAYING --> PLAYING: Data transmission
    PLAYING --> READY: PAUSE request
    READY --> PLAYING: RESUME request
    PLAYING --> READY: TEARDOWN request
    READY --> [*]: Connection closed
    PLAYING --> [*: TEARDOWN request

    state "TRANSPORT SETUP" as SETUP
    state "DATA TRANSMISSION" as PLAY
    state "CLOSE CONNECTION" as TEARDOWN
```

#### RTSP Request Processing Flow

```mermaid
flowchart TD
    A[Receive RTSP request] --> B[Parse request line]
    B --> C["Parse Headers CSeq, User-Agent"]
    C --> D{Request type}

    D-->|OPTIONS| E[Return supported options]
    D-->|DESCRIBE| F["Generate SDP with SPS/PPS"]
    D-->|SETUP| G["RTP/RTCP channel allocation"]
    D-->|PLAY| H[RTP data stream starts]
    D-->|TEARDOWN| I[Stop stream, release resources]

    E --> J[Send 200 OK]
    F --> K[Encoding params -> Base64]
    G --> L[RTP_MTU=1400, SSRC=random]
    H --> M[Timestamp synchronization]
    I --> N[Close client connection]

    J --> O[RTSP state machine update]
    K --> O
    L --> O
    M --> O
    N --> END[End]
```

#### RTP Fragmentation Implementation

```mermaid
flowchart TD
    A[Receive NAL unit] --> B{"NAL size > 1400?"}
    B-->|Yes| C[Start FU-A fragmentation]
    B-->|No| D[Single packet send]

    C --> E["Create FU-A header: F=1 NRI=original NRI TYPE=28"]
    E --> F[First packet: S=1, E=0]
    F --> G["Fragment payload <=1380 bytes"]
    G --> H{More data?}
    H-->|Yes| I[Middle packet: S=0, E=0]
    H-->|No| J[Last packet: S=0, E=1]
    I --> G

    J --> K[RTP packet: header + FU-A + payload]
    K --> L[TCP interleaved transport]
    L --> M[Update RTP sequence number]

    D --> N[RTP packet: original NAL]
    N --> M
```

### 2. Encoded Stream Publisher (`platform/camera-daemon/src/encoded_publisher.cpp`)

#### Dual-Thread Architecture Design

```mermaid
flowchart TD
    subgraph "Encode Callback Thread - Fast enqueue, non-blocking"
        A["Encode callback on_packet"] --> B[Format check]
        B --> C[Queue check]
        C --> D{"V1(22B)/V2(30B)?"}
        D --> V2
        D --> V1
        V1 --> E[V1 Header: 4B size + 1B codec + 1B flags + 8B PTS]
        V2 --> F[V2 Header: 4B size + 1B codec + 1B flags + 8B PTS + 8B DTS]
        E --> G[Push to queue]
        F --> G
        G --> H[Condition notify dispatch thread]
    end

    subgraph "Dispatch Thread - Independent processing, avoids blocking"
        I[Wait for queue condition] --> J[Dequeue frame data]
        J --> K[Keyframe detection]
        K --> L[UDP/TCP broadcast]
        L --> M[Stats update]
    end

    H --> I
    M --> N{More data?}
    N-->|Yes| J
    N-->|No| O[Sleep wait]
    O --> I
```

#### Encoded Publisher V2 Protocol Format

```mermaid
flowchart LR
    subgraph "Encoded Publisher V2 Header (30 bytes)"
        direction TB
        offset0[0-3: Total size<br>4 bytes Little-Endian]
        offset4[4: Codec type<br>0=H264, 1=H265]
        offset5[5: Flags<br>bit0=keyframe]
        offset6[14: PTS<br>8 bytes ns]
        offset22[22: DTS<br>8 bytes ns]
        offset30[30: Reserved<br>Unused]
    end

    subgraph "Annex-B Payload"
        direction TB
        sps[SPS: 0x00 00 00 01 67...]
        pps[PPS: 0x00 00 00 01 68...]
        nal[NAL: 0x00 00 00 01 65...]
    end

    offset30 --> spps[...]
    sps --> pps
    pps --> nal
```

#### Keyframe Detection Algorithm

```mermaid
flowchart TD
    A[Input Annex-B data] --> B[Find start code<br>0x000001/0x00000001]
    B --> C[Locate NAL start position]
    C --> D{H.264/H.265?}

    D-->|H.264| E["Read NAL type nal_byte AND 0x1F"]
    D-->|H.265| F["Read NAL type nal_byte>>1 AND 0x3F"]

    E --> G{nal_type == 5?}
    F --> H{"type in 16,23?"}

    G-->|Yes| I[Mark as keyframe]
    G-->|No| J[Continue searching]
    H-->|Yes| I
    H-->|No| J

    I --> K[Return true]
    J --> B
```

### 3. H.264 Stream API (`platform/platform-api/handlers/h264_stream.go`)

#### WebSocket Connection Handling Flow

```mermaid
flowchart TD
    A[New WebSocket connection] --> B[Create H264Stream instance]
    B --> C[Start readLoop]
    C --> D[Connect to UDS: /run/aipc/encoded/{name}.sock]
    D --> E{Connection successful?}

    E-->|Yes| F[Send cached SPS/PPS]
    E-->|No| G[Exponential backoff retry]

    F --> H[Receive frame data]
    G --> I[Delay 1s/2s/4s/8s/10s]
    I --> D

    H --> J[Protocol version detection]
    J --> K{total_size >= 30?}

    K-->|V1| L[Read 22-byte header]
    K-->|V2| M[Read 30-byte header]

    L --> N[Extract PTS]
    M --> O[Extract PTS + DTS]

    N --> P[Split Annex-B]
    O --> P

    P --> Q[Convert to AVCC]
    Q --> R[Broadcast to all clients]

    R --> S[Track keyframes]
    S --> T{Exceeds GOP-1?}
    T-->|Yes| U[Request IDR frame]
    T-->|No| V[Continue processing]

    U --> R
    V --> H
```

#### Annex-B to AVCC Conversion

```mermaid
flowchart TD
    A[Annex-B data] --> B[Find start code positions]
    B --> C[Extract NAL units]
    C --> D{NAL type?}

    D-->|"SPS(7)"| E[AVCC: 4-byte length + SPS]
    D-->|"PPS(8)"| F[AVCC: 4-byte length + PPS]
    D-->|"IDR(5)"| G[AVCC: 4-byte length + IDR]
    D-->|Other| H[AVCC: 4-byte length + NAL]

    E --> I[Add to SPS/PPS cache]
    F --> I
    G --> J[Mark as keyframe]
    H --> K[Regular data packet]

    I --> L[Broadcast complete frame]
    J --> L
    K --> L
```

#### Frontend Player Architecture

```mermaid
flowchart TD
    A[WebSocket receive] --> B[Unpack V2 Header]
    B --> C[Extract PTS/DTS]
    C --> D[Split AVCC NAL]

    subgraph "WebCodecs Path - Preferred"
        D --> E[Create VideoDecoder]
        E --> F[Decoder configuration]
        F --> G[Decode NAL units]
        G --> H[Get video frames]
    end

    subgraph "MSE Path - Fallback"
        D --> I[Create MediaSource]
        I --> J["appendBuffer append data"]
        J --> K[SourceBuffer processing]
        K --> L[Trigger timeupdate]
    end

    H --> M[Canvas rendering]
    L --> M

    M --> N[Display statistics]
    N --> O[FPS calculation display]
    N --> P[Latency monitoring]
    N --> Q[Bandwidth estimation]

    O --> R[Update UI state]
    P --> R
    Q --> R
```

### 4. Stream API (`platform/platform-api/handlers/stream.go`)

#### RESTful Interface Design

| Endpoint | Method | Function | Parameters |
|----------|--------|----------|------------|
| `/api/v1/media/status` | GET | Stream status and config | - |
| `/api/v1/media/rtsp` | PUT | Enable/disable RTSP | `enabled: bool` |
| `/api/v1/media/encoder` | PUT | Hot update encoding params | `bitrate`, `fps`, `gop` |
| `/api/v1/media/encoder/reconfig` | PUT | Full reconfiguration | `width`, `height`, `codec` |
| `/api/v1/media/streams/:name/enable` | POST | Enable stream | - |
| `/api/v1/media/streams/:name/disable` | DELETE | Disable stream | - |
| `/api/v1/h264/:stream_id` | WebSocket | H.264 stream transport | V1/V2 auto-detection |

#### Hot Update vs Full Reconfiguration Comparison

| Operation Type | Hot Update | Full Reconfiguration |
|---------------|------------|---------------------|
| Impact | Seamless, < 50ms | Restart encoder, ~100ms |
| Supported params | Bitrate, frame rate, GOP | Resolution, codec format |
| User experience | No interruption | Brief black screen |
| Use case | Dynamic quality adjustment | Resolution switching |

### 5. Frontend Playback System (`web/src/pages/media/`)

#### Multi-Stream Playback Architecture

```mermaid
flowchart TD
    A[Main Window] --> B[Main Stream - 1080p]
    A --> C[Sub Stream - 720p]
    A --> D[Third Stream - 480p]

    B --> E[WebCodecs Decoder]
    C --> E
    D --> E

    E --> F[Canvas Compositing Layer]
    F --> G[Video Display Area]
    F --> H[OSD Info Overlay]

    subgraph Control Panel
        I[PTZ Control] --> J[Pan/Tilt Direction]
        K[Image Params] --> L[Brightness/Contrast]
        K --> M[Saturation/Hue]
        N[Stream Management] --> O[Switch Stream]
        N --> P[Fullscreen Display]
    end

    I --> G
    K --> G
    N --> G

    subgraph Monitoring Panel
        Q[Real-time Stats] --> R[FPS: 30]
        Q --> S[Latency: 80ms]
        Q --> T[Bandwidth: 4Mbps]
        U[Alert Info] --> V[Motion Detection]
        U --> W[Scene Occlusion]
    end

    Q --> V
    U --> V
```

## Key Technical Details

### 1. Performance Optimization Parameters

#### RTSP Server Configuration
- **epoll timeout**: 500ms (balance responsiveness and resources)
- **Send buffer**: 4MB (avoid TCP blocking)
- **Session ID**: 16-byte hex random string
- **RTP_MTU**: 1400 bytes (IP + TCP + RTP + FU-A overhead)
- **Max clients**: 8 concurrent connections

#### Encoded Publisher Configuration
- **Protocol version**: V1 (22-byte) / V2 (30-byte) auto-detection
- **Queue size**: Unlimited (avoid frame drops)
- **Dispatch thread**: Runs independently, does not block encoding
- **Keyframe interval**: Request IDR frame every GOP-1 frames

#### WebSocket Stream Configuration
- **Auto-reconnect**: Exponential backoff 1s -> 2s -> 4s -> 8s -> 10s
- **Buffer**: Dynamically adjusted (avoid memory leaks)
- **Heartbeat**: 30-second interval
- **Timeout**: Auto-disconnect after 5 minutes of no data

### 2. Error Handling Strategy

```mermaid
flowchart TD
    A[Connection error] --> B{Error type}
    B-->|UDS disconnected| C[Exponential backoff reconnect]
    B-->|WebSocket failed| D[Remove client]
    B-->|Encoder error| E[Restore encoding state]

    C --> F{Retry count < 10?}
    F-->|Yes| G[Wait with increasing delay]
    F-->|No| H[Mark stream as unavailable]

    D --> I[Attempt auto-reconnect]
    E --> J[Reinitialize encoder]

    G --> C
    I --> C
    J --> K[Continue encoding flow]

    H --> L[Notify frontend of error]
    K --> C
    L --> M[Frontend shows offline]
```

### 3. Cache Management Strategy

#### SPS/PPS Cache Mechanism
- **Trigger condition**: Clear cache when SPS NAL type = 7
- **Cache policy**: Keep latest SPS + PPS combination
- **New clients**: Send cached SPS/PPS immediately upon connection
- **Resolution switch**: Auto-refresh cache to prevent stale data

#### Sequence Number Management
- **Global counter**: Independently maintained per stream ID
- **Monotonically increasing**: Ensures frame order is preserved
- **Persistent**: Maintains continuity across stream rebuilds
- **64-bit width**: Avoids overflow issues

### 4. Timestamp Processing

#### Time Synchronization Mechanism
- **90kHz clock**: RTP standard time base
- **PTS/DTS separation**: V2 protocol supports B-frames
- **Conversion formula**: ns -> 90kHz (divide by 11111.111...)
- **Time base**: First frame set to 0

#### Latency Control
- **End-to-end target**: < 100ms
- **Encoding latency**: ~20ms (determined by GOP size)
- **Network latency**: ~30ms (local network optimized)
- **Rendering latency**: ~30ms (WebCodecs optimized)

## Configuration Examples

### Encoder Configuration
```yaml
encoder:
  main:
    codec: h264
    width: 1920
    height: 1080
    bitrate: 4000000  # 4Mbps
    fps: 30
    gop: 30          # 1-second keyframe interval
    profile: high
    level: 4.1

  sub:
    codec: h264
    width: 1280
    height: 720
    bitrate: 2000000  # 2Mbps
    fps: 25
    gop: 25
```

### RTSP Configuration
```yaml
rtsp:
  enabled: true
  port: 8554
  max_clients: 8
  buffer_size: 4194304  # 4MB
  timeout: 500          # 500ms
```

### WebSocket Configuration
```yaml
websocket:
  path: /api/v1/h264
  max_reconnect_delay: 10
  initial_reconnect_delay: 1
  idle_timeout: 300  # 5 minutes
  ping_interval: 30  # 30 seconds
```

## Monitoring and Diagnostics

### Key Metrics
- **FPS real-time display**: Current frame rate
- **Latency measurement**: End-to-end latency
- **Bandwidth statistics**: Real-time transfer rate
- **Buffer water level**: Decoder buffer usage
- **Error count**: Connection interruption count

### Debug Information
- **NAL unit types**: Encoder output NAL types
- **PTS/DTS delta**: B-frame timestamp difference
- **Keyframe interval**: Actual IDR frame interval
- **Reconnect logs**: Reconnect timestamps and attempt count

## Troubleshooting Guide

### Common Issues
1. **Black screen**: Check if SPS/PPS are sent correctly
2. **Stuttering**: Check network bandwidth and encoding bitrate
3. **Corrupted frames**: Check NAL unit integrity
4. **High latency**: Check GOP size and network latency

### Diagnostic Tools
- **tcpdump**: Capture RTSP/RTP packets
- **wireshark**: Analyze WebSocket streams
- **Frontend console**: Check WebCodecs errors
- **System logs**: View server-side error messages

This implementation fully embodies the design principles of a high-performance, low-latency, fault-tolerant media streaming system, suitable for industrial-grade video monitoring applications.