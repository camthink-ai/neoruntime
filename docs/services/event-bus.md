# Event Bus Service

## Overview

`event-bus` is a local Pub/Sub message bus responsible for AI inference result distribution, application event reporting, and system event notification. It supports MQTT-style wildcard Topic matching.

Tech stack: Go + gRPC, pure in-memory implementation, no external dependencies.

## Directory Structure

```
platform/event-bus/
├── proto/
│   ├── event.proto           # gRPC definitions
│   ├── event.pb.go           # Generated code
│   └── event_grpc.pb.go      # Generated gRPC code
└── server/
    ├── main.go               # Service implementation
    └── main_test.go          # Tests
```

## Architecture Diagram

```mermaid
graph TB
    subgraph "Publishers"
        P1["AI Runtime<br/>Inference Results"]
        P2["Device Control<br/>Device Events"]
        P3["App Manager<br/>Application Events"]
        P4["User Applications<br/>Custom Events"]
    end

    subgraph "Event Bus Service"
        EB["EventBus<br/>gRPC Server"]
        subgraph "Dual Listener Mode"
            US["Unix Socket<br/>Go Clients"]
            TS["TCP Socket<br/>C++ Clients"]
        end
        subgraph "Core Components"
            PS["Publisher Manager"]
            SM["Subscriber Manager"]
            TM["Topic Matcher"]
            ST["Statistics System"]
        end
    end

    subgraph "Subscribers"
        S1["platform-api<br/>Web API"]
        S2["Web Console<br/>Real-time Updates"]
        S3["User Applications<br/>Event Processing"]
        S4["Monitoring Service<br/>Log Collection"]
    end

    subgraph "Features"
        Q["In-memory Queue<br/>1000 msgs/subscriber"]
        W["Worker Threads<br/>4 threads"]
        B["Batch Delivery<br/>10 msgs/batch"]
        R["Read-Write Lock<br/>Concurrency Safe"]
    end

    P1 -->|Publish| EB
    P2 -->|Publish| EB
    P3 -->|Publish| EB
    P4 -->|Publish| EB

    EB --> US
    EB --> TS

    PS -->|Message Delivery| SM
    SM -->|Topic Matching| TM
    TM -->|Match Results| SM

    EB -->|Stream Push| S1
    EB -->|Stream Push| S2
    EB -->|Stream Push| S3
    EB -->|Stream Push| S4

    PS -.->|Stats Update| ST
    EB -.->|Performance Metrics| W
    EB -.->|Queue Management| Q
    EB -.->|Batch Processing| B
    EB -.->|Concurrency Control| R
```

## gRPC API

Service name: `EventBus`, listening on `unix:///run/aipc/event-bus.sock` (TCP: `127.0.0.1:50053`).

### Pub/Sub Message Flow

```mermaid
sequenceDiagram
    participant P as Publisher
    participant EB as EventBus Server
    participant SM as Subscriber Manager
    participant S as Subscriber
    participant Q as Message Queue

    Note over P: Publish event

    P->>EB: gRPC Publish(topic="model/detected")
    EB->>EB: Generate event_id
    EB->>EB: Set timestamp_ns
    EB->>SM: findMatchingSubscribers()
    SM->>SM: Topic matching algorithm
    SM-->>EB: Return matched subscriber list

    loop For each subscriber
        EB->>Q: Deliver to subscriber queue
        Q-->>S: Non-blocking delivery
        alt Queue not full
            S->>S: Process event
        else Queue full
            Note over EB: Drop old messages
        end
    end

    EB->>EB: Update statistics counters
    EB-->>P: PublishResponse
```

### Topic Wildcard Matching Flowchart

```mermaid
flowchart TD
    A[Receive Topic and pattern] --> B[Split path segments]
    B --> C{Pattern type}

    C -->|Exact match| D[Full equality check]
    C -->|Single-level wildcard| E["* matches single segment"]
    C -->|Multi-level wildcard| F["** matches multiple segments"]
    C -->|Suffix match| G["**/suffix format"]

    D --> H[Return match result]
    E --> I["Check if segment count is equal"]
    F --> J["Recursively match remaining parts"]
    G --> K["Match from end backwards"]

    H --> L[Return match result]
    I --> L
    J --> L
    K --> L

    style B fill:#e3f2fd
    style E fill:#f3e5f5
    style F fill:#e8f5e9
    style G fill:#fff3e0
```

### Dual Listener Mode Architecture

```mermaid
graph LR
    subgraph "EventBus Service"
        subgraph "Service Instance"
            EB["EventBus Server<br/>Go gRPC Service"]
        end

        subgraph "Listeners"
            US["Unix Socket Listener<br/>/run/aipc/event-bus.sock<br/>Low overhead, Go clients"]
            TS["TCP Listener<br/>127.0.0.1:50053<br/>C++ client compatible"]
        end

        EB --> US
        EB --> TS
    end

    subgraph "Client Types"
        GC["Go Client<br/>Uses Unix Socket<br/>Performance priority"]
        CC["C++ Client<br/>Uses TCP Socket<br/>Compatibility priority"]
    end

    GC --> US
    CC --> TS

    ```

## gRPC API

| RPC | Request | Response | Description |
|-----|---------|----------|-------------|
| `Publish` | `PublishRequest` | `PublishResponse` | Publish single event |
| `PublishBatch` | `stream PublishRequest` | `Status` | Batch publish |
| `Subscribe` | `SubscribeRequest` | `stream Event` | Subscribe to Topic (wildcard supported) |
| `Unsubscribe` | `SubscribeRequest` | `Status` | Unsubscribe |
| `ListTopics` | `Empty` | `TopicListResponse` | List active Topics |
| `GetTopicInfo` | `TopicInfo` | `TopicInfo` | Topic details |
| `GetStats` | `Empty` | `SystemStats` | System statistics |
| `GetTopicStats` | `TopicInfo` | `EventStats` | Topic statistics |

### Key Message Structures

**Event**:
```protobuf
message Event {
  string topic = 1;           // Topic (e.g. "model/person_v1/detections")
  uint64 timestamp_ns = 2;    // Timestamp
  string source = 3;          // Source (app_id / service_name)
  string event_id = 4;        // Event ID (auto-generated)
  bytes payload = 10;         // Payload (JSON or protobuf)
  string payload_type = 11;   // "json" / "protobuf"
  map<string, string> metadata = 20;
}
```

**SubscribeRequest**:
```protobuf
message SubscribeRequest {
  string topic = 1;           // Wildcard supported
  string subscriber_id = 2;   // Subscriber ID
  map<string, string> filters = 10;  // Optional filters
  uint32 queue_size = 20;     // Queue size
  bool drop_old = 21;         // Drop old messages when queue is full
}
```

**Statistics**:
```protobuf
message EventStats {
  string topic = 1;
  uint64 published_count = 2;
  uint64 delivered_count = 3;
  uint64 dropped_count = 4;
  float avg_latency_us = 5;
}

message SystemStats {
  repeated EventStats topic_stats = 1;
  uint32 total_subscribers = 2;
  uint32 total_topics = 3;
  uint64 uptime_ms = 4;
}
```

## Configuration

Configuration file: `configs/platform/event-bus.yaml`

```yaml
service:
  name: event-bus
  listen: unix:///run/aipc/event-bus.sock
  tcp_listen: "127.0.0.1:50053"    # C++ gRPC clients (unix: not supported)
  log_level: info

bus:
  queue_size: 1000          # Per-subscriber queue size
  max_topics: 1000
  workers: 4                # Worker thread count
  batch_size: 10            # Batch delivery size
  inactive_topic_ttl: 3600  # Inactive Topic cleanup interval (seconds)

routing:
  priorities:
    "system/": 10           # Highest priority
    "alert/": 8
    "model/": 5
    "app/": 5
  rate_limits:
    "model/*": 1000         # 1000 msg/sec
    "app/*": 100            # 100 msg/sec

monitoring:
  stats_enabled: true
  stats_interval_sec: 10
  metrics_port: 9091
```

## Topic Wildcard Matching

Supports MQTT-style wildcards:

| Pattern | Description | Example |
|---------|-------------|---------|
| Exact match | Exact Topic match | `app/test/alert` |
| `*` | Matches single level | `app/*/alert` matches `app/test/alert` |
| `**` | Matches multiple levels | `app/**` matches `app/a/b/c` |
| `**/suffix` | Suffix match | `app/**/events` matches `app/a/b/events` |

**Matching rules**:
- `*` matches exactly one path segment (does not cross `/`)
- `**` matches zero or more path segments
- Recursive segment-by-segment matching algorithm

### Topic Matching Algorithm Details

```mermaid
flowchart TD
    A["Input: topic=a/b/c/d"] --> B[Split topic]
    B --> C{Pattern type}
    C -->|** wildcard| D[Segment processing]

    D --> E["First segment a"]
    E --> F["Exact match 'a' == 'a'"]
    F --> G["Remaining topic: b/c/d"]
    F --> H["Remaining pattern: **/d"]

    G --> I{Is **?}
    I -->|Yes| J["Match any number of segments"]
    I -->|No| K["Exact match remaining parts"]

    J --> L["Match b/c portion"]
    L --> M["Last segment d == d"]
    M --> N[Return match success]

    C -->|Exact match| O[Full equality comparison]
    C -->|Single-level wildcard| P["Check if segment count is equal"]

    style D fill:#e3f2fd
    style I fill:#f3e5f5
    style J fill:#e8f5e9
```

## Message Flow

### Publish

1. Auto-generate `event_id` (if not provided)
2. Auto-set `timestamp_ns`
3. Find subscribers via Topic wildcard matching
4. Non-blocking delivery (drop if queue is full)
5. Update statistics counters

### Subscribe

1. Create buffered channel (configurable size)
2. Register subscriber
3. Stream events to client
4. Auto-cleanup on disconnect

## Pure In-Memory Architecture

```mermaid
graph LR
    subgraph "Event Bus Memory Layout"
        A[Publishers Map] --> B[Topic -> Publisher List]
        C[Subscribers Map] --> D[Topic -> Subscriber List]
        E[Statistics Map] --> F[Topic -> Stats Info]
        G[Message Queues] --> H[Subscriber ID -> Channel]
    end

    subgraph "Performance Optimizations"
        I[Zero-copy pointer passing]
        J[Read-write lock protection]
        K[Batch delivery]
        L[Async non-blocking]
    end

    B -->|Read/Write| I
    D -->|Read/Write| J
    F -->|Atomic update| I
    H -->|Channel delivery| K

    style A fill:#e1f5fe
    style C fill:#e1f5fe
    style E fill:#e1f5fe
    style G fill:#e1f5fe

    style I fill:#fff3e0
    style J fill:#fff3e0
    style K fill:#fff3e0
    style L fill:#fff3e0
```

## Performance Characteristics

- **Pure in-memory**: No external dependencies, zero disk I/O
- **Zero-copy**: Internal event pointer passing
- **Async publish**: Immediate return, does not wait for delivery
- **Slow consumer handling**: Drop old messages when queue is full
- **Read-write lock**: Allows multiple concurrent publishers
- **Low latency**: < 1ms end-to-end

### Performance Data

| Metric | Value | Description |
|--------|-------|-------------|
| Single message publish latency | < 0.1ms | Including matching and delivery |
| Max throughput | 100,000 msg/s | Batch publish mode |
| Memory usage | < 50MB | Including subscriptions and statistics |
| Max Topics | 1000 | Configurable |
| Queue size | 1000 msgs/subscriber | Configurable |

## CLI Tool

```bash
# List Topics
aipc-cli event topics

# Publish event
aipc-cli event publish app/test '{"msg":"hello"}'

# Subscribe to events (wildcard supported)
aipc-cli event subscribe 'model/*/detections'

# View statistics
aipc-cli event stats
```

### Internal Statistics Management

```mermaid
stateDiagram-v2
    [*] --> Initialize

    Initialize --> Collect : Every 10 seconds
    Collect --> Calculate : Update counters
    Calculate --> Store : Write to memory
    Store --> Collect : Next cycle

    Collect -->|Exceeds max| Cleanup
    Cleanup --> Delete : Clean inactive Topics
    Cleanup --> Collect
```

## Error Handling

```mermaid
flowchart TD
    A[Publish request] --> B{Validate Event}
    B -->|Invalid| C[Return error]
    B -->|Valid| D[Process publish]

    D --> E{Find subscribers}
    E -->|No match| F[Update stats only]
    E -->|Matched| G[Deliver message]

    G --> H{Queue status}
    H -->|Not full| I[Successful delivery]
    H -->|Full| J[Drop old messages]

    I --> K[Update success stats]
    J --> L[Update drop stats]
    K --> M[Return success]
    L --> M

    style C fill:#ffebee
    style J fill:#fff3e0
```