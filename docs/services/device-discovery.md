# Device Discovery Service

## Overview

`device-discovery` is a Go gRPC service implementing the CT-Disc (CamThink Device Discovery & Management) protocol for automatic discovery and management of CamThink devices on the network.

## Directory Structure

```
platform/device-discovery/
├── server/main.go       # Service entry point
├── discovery/
│   ├── ctdisc.go        # CT-Disc protocol definitions
│   ├── listener.go      # UDP multicast listener
│   ├── announcer.go     # UDP multicast announcer (device-side broadcast)
│   ├── registry.go      # Device registry
│   └── errors.go        # Error definitions
├── handler/handler.go   # gRPC handler
└── proto/
    ├── discovery.proto
    ├── discovery.pb.go
    └── discovery_grpc.pb.go
```

## System Architecture Diagram

```mermaid
graph TB
    subgraph "CamThink Device Layer"
        NE301["NE301 (STM32)<br/>Ethernet"]
        NE503["NE503 (Linux)<br/>Ethernet/WiFi"]
        NE101_WIFI["NE101 (ESP32-S3)<br/>WiFi/HaLow"]
        NE101_CAT1["NE101 (ESP32-S3)<br/>CAT1 Cellular"]
    end

    subgraph "Network Transport Layer"
        MC["UDP Multicast<br/>239.255.255.250:19850"]
        MQ["MQTT Broker<br/>CamThink Platform"]
        MD["mDNS (Optional)<br/>_aipc._tcp.local"]
    end

    subgraph "Discovery Service"
        DS["DeviceDiscovery<br/>gRPC Server"]
        subgraph "Listeners"
            ML["Multicast Listener<br/>UDP multicast listener"]
        end
        subgraph "Announcer"
            AN["Multicast Announcer<br/>Periodic ct-announce broadcast"]
        end
        subgraph "Registry"
            REG["Registry<br/>Device information storage"]
        end
        subgraph "State Manager"
            SM["Timeout Checker<br/>Timeout detection"]
        end
        subgraph "gRPC Handler"
            GH["Handler<br/>gRPC API implementation"]
        end
    end

    subgraph "Management Side"
        PA["platform-api"]
        WC["Web Console"]
    end

    NE301 -->|ct-announce| ML
    NE503 -->|ct-announce| ML
    NE101_WIFI -->|ct-announce| ML
    NE101_CAT1 -->|ct-register| MQ

    NE101_WIFI -.->|Optional| MD
    NE503 -.->|Optional| MD

    ML -->|Receive multicast| DS
    AN -->|Send ct-announce| ML
    MQ -->|MQTT messages| DS
    DS -->|Update| REG
    DS -->|Detect timeout| SM

    ML -->|UDP Listen| ML
    SM -->|Periodic check| REG
    GH -->|gRPC API| PA
    PA -->|REST API| WC

    DS -->|Device events| PA

    style ML fill:#e3f2fd
    style REG fill:#e8f5e9
    style SM fill:#f3e5f5
    style GH fill:#fff3e0
```

## CT-Disc Protocol Interaction Flow

### Scenario 1: Device Power-On Auto Discovery (Multicast Mode)

```mermaid
sequenceDiagram
    participant D as Device (NE301/NE503/NE101)
    participant ML as Multicast Listener
    participant REG as Registry
    participant DS as Discovery Service
    participant GH as Handler

    Note over D: Device powered on, network ready

    D->>D: Initialize CT-Disc
    D->>D: Start timer (5s)

    loop Every 5 seconds
        D->>ML: UDP multicast ct-announce
        Note over D: 239.255.255.250:19850

        ML->>ML: Parse JSON packet
        ML->>REG: Registry.Update()

        alt New device
            REG->>REG: Create device record
            REG->>DS: Trigger ONLINE event
            DS->>GH: gRPC event push
        else Existing device
            REG->>REG: Update last_seen
        end
    end
```

### Scenario 2: Device State Management

```mermaid
stateDiagram-v2
    [*] --> ONLINE : Announce/register received

    ONLINE --> ONLINE : Periodic last_seen refresh
    ONLINE --> OFFLINE : 30s without message
    OFFLINE --> ONLINE : Message received again
    OFFLINE --> DELETED : Manual delete

    ONLINE --> UPDATING : IP address change
    UPDATING --> ONLINE : Update complete

    state "OFFLINE State" as OfflineState {
        [*] --> Waiting Timeout
        Waiting Timeout --> ON_TIMEOUT : Timeout detected
    }

    state "ONLINE State" as OnlineState {
        [*] --> ONLINE
        ONLINE --> ON_REFRESH : Refresh timer
    }
```

### Scenario 3: Multi-Protocol Device Unified Management

```mermaid
flowchart TB
    A[Device Online] --> B{Network Type}

    B -->|Ethernet/WiFi/HaLow| C[Multicast Discovery]
    B -->|CAT1 Cellular| D[MQTT Discovery]

    C --> E[Parse ct-announce]
    D --> F[Parse ct-register]

    E --> G[Registry stores by SN]
    F --> G

    G --> H[Unified Device Model]
    H --> I[Event Push]
    H --> J[gRPC Query]
    H --> K[Web Management UI]

    style C fill:#e3f2fd
    style D fill:#f3e5f5
    style G fill:#e8f5e9
```

## gRPC API

Service name: `aipc.discovery.DiscoveryService`, listening on `unix:///run/aipc/device-discovery.sock`.

### ListDevices Flow

```mermaid
sequenceDiagram
    participant C as gRPC Client<br/>(platform-api / ct-disc)
    participant GH as Handler
    participant REG as Registry
    participant ML as Multicast Listener

    C->>GH: gRPC ListDevices()
    GH->>REG: Query device list
    REG->>REG: Filter by product/status
    REG-->>GH: Return device list
    GH-->>C: Device data

    Note over REG: Registry structure:<br/>map[SN] -> DeviceInfo
```

> Note: the `DiscoveryHandler` gRPC client exists in platform-api, but no
> `/api/v1/discovery/*` HTTP routes are currently registered — discovery is
> consumed directly via gRPC (or by the `ct-disc` CLI tool).

### Manual Scan Flow

```mermaid
flowchart TD
    A[User triggers scan] --> B[gRPC TriggerScan]
    B --> C[Send multicast Probe]
    C --> D[Wait for device responses]

    D --> E{Response Collection}
    E -->|Timeout| F[Return results]
    E -->|Response received| G[Process new device]

    G --> H[Update Registry]
    H --> I[Trigger ONLINE event]
    I --> F

    F --> J[Return scan results]
    J --> K[Web UI update]

    style C fill:#e3f2fd
    style G fill:#e8f5e9
```

## CT-Disc Protocol Specification

> The canonical wire-level protocol spec (message types, MQTT commands,
> state machine, capability identifiers, product defaults) is maintained in
> [`../references/CT_DISC_PROTOCOL.md`](../references/CT_DISC_PROTOCOL.md).
> This section is an abridged summary tied to the `device-discovery`
> implementation.

### Multicast Transport Parameters

| Parameter | Value |
|-----------|-------|
| Transport | UDP Multicast |
| Multicast Address | `239.255.255.250` |
| Target Port | `19850` |
| Broadcast Interval | `5000ms` |
| Max Packet Size | `512 bytes` |

### MQTT Transport Parameters

> The `device-discovery` service itself speaks UDP multicast only. The MQTT
> transport below is used by the **`ct-disc` CLI tool** to send commands to
> devices (and by the NE101 CAT1 device side); the service does not subscribe
> to the broker.

| Parameter | Value |
|-----------|-------|
| Register Topic | `ct/disc/register` |
| Command Topic | `ct/cmd/{sn}` |
| Response Topic | `ct/resp/{sn}` |
| Register QoS | `0` |
| Command QoS | `1` |
| Register Interval | `30000ms` |

### JSON Payload Format

**Multicast Announce**:
```json
{
    "type": "ct-announce",
    "product": "NE101",
    "sn": "CT101-2026-00001",
    "mac": "AA:BB:CC:DD:EE:FF",
    "ip": "192.168.1.50",
    "fw": "v1.0.0",
    "port": 80,
    "hw": "ESP32-S3",
    "caps": ["camera", "mqtt", "http"]
}
```

**MQTT Register**:
```json
{
    "type": "ct-register",
    "product": "NE101",
    "sn": "CT101-2026-00001",
    "mac": "AA:BB:CC:DD:EE:FF",
    "ip": "10.0.1.50",
    "fw": "v1.0.0",
    "port": 80,
    "hw": "ESP32-S3",
    "caps": ["camera", "mqtt", "http", "cellular"],
    "net": "cat1"
}
```

### Device State Management

```mermaid
flowchart LR
    A[Multicast Listening] --> B[Receive Message]
    B --> C[Parse JSON]
    C --> D{Device exists?}

    D -->|No| E[Create new record]
    D -->|Yes| F[Update last_seen]

    E --> G[State: ONLINE]
    F --> G

    G --> H[Trigger event]
    H --> I[Update statistics]

    style B fill:#e3f2fd
    style E fill:#e8f5e9
    style F fill:#f3e5f5
```

## gRPC API Interfaces

### ListDevices

```protobuf
rpc ListDevices(ListDevicesRequest) returns (ListDevicesResponse)
```

| Field | Type | Description |
|-------|------|-------------|
| **Request** | | |
| `product` | string | Filter by product model, empty string for no filter |
| `status` | DeviceStatus | Filter by device status |
| **Response** | | |
| `devices` | DiscoveredDevice[] | Device list |

### GetDevice

```protobuf
rpc GetDevice(GetDeviceRequest) returns (DiscoveredDevice)
```

| Field | Type | Description |
|-------|------|-------------|
| **Request** | | |
| `serial_number` | string | Device serial number |

### TriggerScan

```protobuf
rpc TriggerScan(TriggerScanRequest) returns (TriggerScanResponse)
```

| Field | Type | Description |
|-------|------|-------------|
| **Request** | | |
| `timeout_seconds` | int32 | Scan timeout |
| **Response** | | |
| `found_count` | int32 | Number of devices found |
| `new_devices` | DiscoveredDevice[] | New device list |

### WatchDevices

```protobuf
rpc WatchDevices(WatchDevicesRequest) returns (stream DeviceEvent)
```

| Field | Type | Description |
|-------|------|-------------|
| **Event** | | |
| `type` | EventType | `ONLINE` / `OFFLINE` / `UPDATED` |
| `device` | DiscoveredDevice | Device information |

> There is **no** `SendCommand` gRPC RPC in the `DiscoveryService` service
> (the proto defines only `ListDevices`, `GetDevice`, `TriggerScan`, and
> `WatchDevices`). Device commands are sent out-of-band by the `ct-disc` CLI
> tool over MQTT — see [MQTT Command Format](#mqtt-command-format).

## Network Topology Diagram

```mermaid
graph TB
    subgraph "LAN Environment"
        subgraph "Same Subnet Devices"
            NE301["NE301<br/>STM32N6570"]
            NE503["NE503<br/>Hailo-15"]
            NE101["NE101<br/>ESP32-S3"]
        end

        MC["239.255.255.250:19850<br/>UDP Multicast"]
    end

    subgraph "WAN Environment"
        NE101_CAT1["NE101 CAT1<br/>Cellular Network"]
        MQTT["CamThink MQTT Broker<br/>(used by ct-disc CLI)"]
    end

    subgraph "Management Side"
        DS["device-discovery<br/>Go Service"]
        PA["platform-api"]
        WEB["Web Console"]
    end

    NE301 -->|ct-announce| MC
    NE503 -->|ct-announce| MC
    NE101 -->|ct-announce| MC

    NE101_CAT1 -->|ct-register| MQTT

    MC -->|Listen| DS
    CTDISC["ct-disc CLI"] -->|MQTT send| MQTT

    DS -->|gRPC| PA
    PA -->|REST| WEB

    style MC fill:#e3f2fd
    style MQTT fill:#f3e5f5
    style DS fill:#e8f5e9
```

## Device-Side MQTT Architecture (NE101 CAT1)

```mermaid
graph TB
    subgraph "NE101 CAT1 Device"
        MQTT_LIB["MQTT Protocol Stack<br/>(Shared)"]

        subgraph "Business Module"
            BIZ["Business MQTT<br/>(User-configured Broker)"]
            BIZ -->|"Connection 1"| MQTT_LIB
        end

        subgraph "CT-Disc Module"
            CTD["CT-Disc MQTT<br/>(Management Platform Broker)"]
            CTD -->|"Connection 2"| MQTT_LIB
        end
    end

    subgraph "External Brokers"
        BIZ_BROKER["Business Broker<br/>(User-configured)"]
        MGMT_BROKER["Management Broker<br/>(CamThink Platform)"]
    end

    BIZ -->|Business data| BIZ_BROKER
    CTD -->|Register/Commands| MGMT_BROKER
    MGMT_BROKER -->|Command push| CTD

    style CTD fill:#e8f5e9
```

## Configuration

Configuration file: `configs/platform/discovery.yaml`

```yaml
service:
  name: device-discovery
  listen: unix:///run/aipc/device-discovery.sock
  log_level: info

discovery:
  multicast_addr: 239.255.255.250
  multicast_port: 19850
  timeout: 30
  interface: ""

announce:
  enabled: true
  product: "NE503"
  port: 8080
  interval: 5
  caps:
    - ai
    - camera
    - http
    - mqtt
```

### MQTT Command Format

**Command Topic: `ct/cmd/{sn}`**
```json
{
    "id": "cmd-20260520-001",
    "action": "reboot",
    "params": {},
    "timestamp": 1716163200
}
```

**Command Response Topic: `ct/resp/{sn}`**
```json
{
    "id": "cmd-20260520-001",
    "result": "ok",
    "data": {},
    "timestamp": 1716163201
}
```

### Standard Management Commands

| action | Description | params | Typical Devices |
|--------|-------------|--------|-----------------|
| `reboot` | Reboot device | `{}` | All |
| `get_info` | Get device details | `{}` | All |
| `set_config` | Push configuration | `{key: value}` | All |
| `ota_upgrade` | OTA upgrade | `{"url": "..."}` | NE101, NE503 |
| `capture` | Trigger photo capture | `{}` | NE101 |
| `set_network` | Modify network config | `{"mode": "static"}` | NE503 |

## Monitoring and Statistics

```mermaid
flowchart TD
    A[Discovery Service] --> B[Device Statistics]
    A --> C[Event Statistics]
    A --> D[Performance Monitoring]

    B --> B1[Total Devices]
    B --> B2[Online Devices]
    B --> B3[Device Distribution Stats]
    B --> B4[Device Survival Rate]

    C --> C1[Event Count]
    C --> C2[Event Type Distribution]
    C --> C3[Event Latency]

    D --> D1[Multicast Receive Rate]
    D --> D2[Memory Usage]
    D --> D3[Response Time]

    style B fill:#e3f2fd
    style C fill:#e8f5e9
    style D fill:#f3e5f5
```

## Build

```bash
make platform  # Build all platform services (including device-discovery)
```

### Startup Flow

```mermaid
flowchart TD
    A["main()"] --> B[Load Config]
    B --> C[Create Registry]
    C --> D[Create Multicast Listener]
    D --> E[Start Listening]
    E --> F[Start Timeout Check]
    F --> G{announce.enabled?}
    G -->|Yes| H[Resolve SN + FW version]
    H --> I[Start Announcer]
    I --> J[Create gRPC Server]
    G -->|No| J
    J --> K[Listen on Unix Socket]
    K --> L[Service Running]

    style D fill:#e3f2fd
    style E fill:#e8f5e9
    style F fill:#f3e5f5
    style I fill:#fff3e0
```

## Desktop Management Tools

### ct-disc CLI

A command-line tool for device discovery and management from a desktop/laptop:

```bash
# Build
cd tools/ct-disc && go build -o ct-disc .

# Commands
ct-disc list                          # List discovered devices
ct-disc list --watch                  # Continuous watch mode
ct-disc scan                          # Active scan for devices
ct-disc scan --iface eth0             # Scan on specific interface
ct-disc send --sn CT503-001 reboot    # Send command via MQTT
```

Source: `tools/ct-disc/`

### ct-disc-gui (Wails Desktop App)

A cross-platform desktop GUI built with Wails v2 (Go backend + React frontend):

```
tools/ct-disc/gui/ct-disc-gui/
├── app.go                    # Wails bindings (discovery, network config)
├── main.go                   # Window configuration
├── frontend/
│   └── src/
│       ├── App.tsx           # Main layout
│       ├── components/
│       │   ├── DeviceTable.tsx         # Device list
│       │   ├── DeviceDetail.tsx        # Device detail panel
│       │   ├── CommandDialog.tsx       # MQTT command dialog
│       │   ├── NetworkConfigDialog.tsx # Device network configuration
│       │   ├── StatusBar.tsx           # Listener stats + device counts
│       │   └── SettingsPanel.tsx       # MQTT broker settings
│       └── hooks/
│           └── useDevices.ts           # Wails event binding hook
└── wails.json
```

**Features:**
- Real-time device list with online/offline indicators
- Network interface selection
- Active scan button
- Device detail panel (SN, product, IP, firmware, capabilities)
- Open device management page in browser
- Send MQTT commands to devices
- Network configuration (DHCP/static IP, gateway, DNS)
- Listener statistics (packet receive count, event count)

**Build:**
```bash
cd tools/ct-disc/gui/ct-disc-gui
wails build -clean -ldflags "-s -w"
# Output: build/bin/ct-disc-gui (Linux/macOS) or build/bin/ct-disc-gui.exe (Windows)
```

**Development:**
```bash
cd tools/ct-disc/gui/ct-disc-gui
wails dev
```
