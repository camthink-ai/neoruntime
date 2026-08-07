# Configuration Reference

## Overview

All services use YAML configuration files, deployed at `/data/aipc/etc/`.

## Configuration File List

| File | Service | Description |
|------|---------|-------------|
| `configs/platform-api.yaml` | platform-api | Web API gateway |
| `configs/platform/app-manager.yaml` | app-manager | Container management |
| `configs/platform/event-bus.yaml` | event-bus | Message bus |
| `configs/platform/device-control.yaml` | device-control | Device control |
| `configs/platform/camera-daemon.yaml` | camera-daemon | Media pipeline |
| `configs/platform/discovery.yaml` | device-discovery | Device discovery |
| `configs/ai/ai-runtime.yaml` | ai-runtime | AI inference |
| `configs/preload.yaml` | — (factory preload manifest) | Factory preload model/app lists bundled into the factory image |

---

## platform-api.yaml

```yaml
service:
  name: platform-api
  http_addr: "127.0.0.1:8080"    # HTTP listen address (behind nginx gateway)
  log_level: debug              # Log level
  log_file: "/var/log/aipc/platform-api.log"

services:
  ai_runtime: "unix:///run/aipc/ai-runtime.sock"
  event_bus: "unix:///run/aipc/event-bus.sock"
  device_control: "unix:///run/aipc/device-control.sock"
  app_manager: "unix:///run/aipc/app-manager.sock"
  camera_control: "unix:///run/aipc/camera-control.sock"

model:
  storage_path: "/data/aipc/models"

storage:
  root_path: "/data/aipc"
  model_blob_path: "/data/aipc/models/blobs"
  min_free_bytes: 104857600     # Minimum free space 100MB

stream:
  camera_config: "/data/aipc/etc/camera-daemon.yaml"
  rtsp_base_url: "rtsp://localhost:8554"
  encoded_pub_dir: "/run/aipc/encoded"

web:
  static_path: "/data/aipc/web"
  enable_cors: true

auth:
  enabled: true
  token_key: ""
  username: "admin"
  password: "password"

database:
  path: "/data/aipc/data/platform.db"
```

---

## app-manager.yaml

```yaml
service:
  name: app-manager
  listen: "unix:///run/aipc/app-manager.sock"
  http_port: 8081
  log_level: info
  log_file: "/var/log/aipc/app-manager.log"

containerd:
  address: "/run/containerd/containerd.sock"
  namespace: aipc
  runtime: io.containerd.runc.v2
  snapshotter: overlayfs

  registry:
    default: registry.aipc.local
    mirrors:
      - https://docker.io

apps:
  registry_path: "/data/aipc/apps/registry"
  instances_path: "/data/aipc/apps/instances"
  manifests_path: "/etc/aipc/apps"
  images_path: "/var/lib/containerd/images"
  logs_path: "/data/aipc/logs/apps"
  log_retention_days: 7

security:
  seccomp_profile: "/etc/aipc/seccomp-default.json"
  readonly_rootfs: true
  no_new_privileges: true
  capabilities_drop:
    - CAP_SYS_ADMIN
    - CAP_NET_ADMIN
    - CAP_SYS_MODULE
    - CAP_SYS_TIME
    - CAP_SYS_BOOT
    - CAP_SYS_NICE
    - CAP_SYS_RESOURCE
    - CAP_SYS_RAWIO
    - CAP_SYS_PTRACE
  namespaces:
    - pid
    - net
    - ipc
    - uts
    - mount
    - user

resources:
  default_cpu_quota: 50         # Single-core percentage
  default_memory_mb: 256
  default_pids_limit: 128
  max_total_cpu_cores: 2
  max_total_memory_gb: 2

healthcheck:
  enabled: true
  interval_sec: 30
  timeout_sec: 5
  auto_restart: true
  max_restart_count: 5
  restart_backoff_sec: 10

network:
  mode: none                    # none | bridge | host
  bridge_name: aipc-br0
  dns_servers:
    - 8.8.8.8
    - 8.8.4.4

ai_runtime:
  enabled: true
  endpoint: "unix:///run/aipc/ai-runtime.sock"
  auto_register_permissions: true

event_bus:
  enabled: true
  endpoint: "unix:///run/aipc/event-bus.sock"
  publish_events:
    - app.installed
    - app.started
    - app.stopped
    - app.crashed
    - app.updated

monitoring:
  enabled: true
  metrics_port: 9092
  stats_interval_sec: 10
  monitor_cpu: true
  monitor_memory: true
  monitor_network: true
  alert_cpu_percent: 90
  alert_memory_percent: 90
```

---

## event-bus.yaml

```yaml
service:
  name: event-bus
  listen: "unix:///run/aipc/event-bus.sock"
  tcp_listen: "127.0.0.1:50053"  # For C++ clients
  log_level: info
  log_file: "/var/log/aipc/event-bus.log"

bus:
  queue_size: 1000              # Per-subscriber queue
  max_topics: 1000
  workers: 4
  batch_size: 10
  inactive_topic_ttl: 3600      # Inactive topic TTL (seconds)
  persist_enabled: false        # Enable message persistence
  persist_path: "/data/aipc/data/event-bus"

routing:
  priorities:
    "system/": 10               # Highest priority
    "alert/": 8
    "model/": 5
    "app/": 5
  rate_limits:
    "model/*": 1000              # msg/sec
    "app/*": 100

monitoring:
  stats_enabled: true
  stats_interval_sec: 10
  metrics_port: 9091

security:
  auth_enabled: false           # Enable authentication
  acl_enabled: true
  acl_file: "/data/aipc/etc/security/event-acl.yaml"
```

---

## device-control.yaml

```yaml
service:
  name: device-control
  listen: "unix:///run/aipc/device-control.sock"
  log_level: info
  log_file: "/var/log/aipc/device-control.log"

camera_daemon:
  lens_endpoint: "/run/aipc/camera-control.sock"

mcu:
  protocol: uart
  device: "/dev/ttyS0"
  baudrate: 921600
  data_bits: 8
  parity: none
  stop_bits: 1
  timeout_ms: 1000
  max_retries: 3
  heartbeat_enabled: true
  heartbeat_interval_sec: 30

capabilities:
  light:
    white_light: true
    ir_led: true
    ir_cut: true
  ptz:
    enabled: true
    pan_range: [-180, 180]      # Degrees
    tilt_range: [-45, 45]
    max_speed: 100
    presets: 16
  lens:
    zoom: true
    focus: true
    autofocus: true
    iris: true
    zoom_range: [1.0, 2.88]    # Optical zoom ratio
    focus_range: [0.1, 10.0]   # Meters
    default_zoom_limit: [-3236, 760]
    default_focus_limit: [-844, 592]
  gpio:
    available_pins: [12, 13, 21, 22]
    input_pins: [12, 13]
    output_pins: [21, 22]

automation:
  day_night_auto:
    enabled: true
    light_sensor_threshold: 300
    hysteresis: 50
    delay_sec: 10
  temperature_protection:
    enabled: true
    warning_temp_c: 75
    critical_temp_c: 85
    action: throttle

event_bus:
  enabled: true
  endpoint: "unix:///run/aipc/event-bus.sock"
  publish_events:
    - gpio_change
    - temperature_alert
    - day_night_switch
```

---

## camera-daemon.yaml

```yaml
hal:
  video_library: "/data/aipc/lib/hal/libaipc_hal.so"
  codec_library: "/data/aipc/lib/hal/libaipc_hal.so"
  lens_library: "/data/aipc/lib/hal/libhal-lens-bridge.so"

media:
  config_path: "/etc/imaging/cfg/hailo15h/imx678/theia_sl410m/4k/medialib_configs/webserver_medialib_config.json"
  backup_path: "/data/aipc/data/media-backup"

video:
  device_path: "/dev/video0"

watchdog:
  scan_interval_ms: 100
  frame_timeout_ms: 5000
  warn_threshold_ms: 3000

rtsp:
  enabled: true

ai_overlay:
  enabled: true
  event_bus_endpoint: "unix:///run/aipc/event-bus.sock"
  topic_prefix: "inference/"
  draw_labels: true
  draw_confidence: true
  draw_landmarks: true
  box_thickness: 2
  stream_map: "third:main,sub:main"

encoders:
  - stream_name: main
    codec: h264
    width: 1920
    height: 1080
    fps: 30
    bitrate: 4000000             # 4Mbps
    gop: 30
    enabled: true
  - stream_name: sub
    codec: h264
    width: 1280
    height: 720
    fps: 30
    bitrate: 2000000             # 2Mbps
    gop: 60
  - stream_name: third
    codec: h264
    width: 640
    height: 384
    fps: 15
    bitrate: 512000              # 512Kbps
    gop: 30

service:
  log_level: debug
  log_file: "/var/log/aipc/camera-daemon.log"
```

---

## discovery.yaml

```yaml
service:
  name: device-discovery
  listen: "unix:///run/aipc/device-discovery.sock"
  log_level: info

discovery:
  multicast_addr: "239.255.255.250"
  multicast_port: 19850
  announce:
    interval: 5                  # Seconds
  timeout: 30                    # Seconds
  interface: ""                  # Empty = all interfaces
```

---

## ai-runtime.yaml

```yaml
service:
  name: ai-runtime
  listen: "unix:///run/aipc/ai-runtime.sock"
  log_level: debug
  log_file: "/var/log/aipc/ai-runtime.log"

hal:
  library_path: "/data/aipc/lib/hal/libaipc_hal.so"
  device_path: "/dev/hailo0"

models:
  repository_path: "/data/aipc/models"
  cache_path: "/var/cache/aipc/models"
  preload: []                    # Model IDs to preload

scheduler:
  global_qps_limit: 100
  global_concurrent_limit: 8
  default_session:
    max_qps: 30
    max_concurrent: 2
    priority: 5
  strategy: fair                 # fair | priority | fifo
  queue_size: 64
  timeout_ms: 5000

fd_receiver:
  socket_path: "/run/aipc/camera.sock"

performance:
  device_mode: high              # high | normal | low
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
  endpoint: "unix:///run/aipc/event-bus.sock"
  auto_publish_results: true
  result_topic_prefix: "inference/"

auto_infer:
  enabled: false
```
