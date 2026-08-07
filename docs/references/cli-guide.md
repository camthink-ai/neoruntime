# CLI Usage Guide

## Overview

`aipc-cli` is the AIPC platform command-line management tool, supporting application management, model management, device control, stream management, event bus, system management, and more.

Version: 0.3.0

## Global Parameters

| Parameter | Shortcut | Description | Default |
|-----------|----------|-------------|---------|
| `--api` | | REST API server address (per-command flag, REST commands only) | `http://localhost:8080` |
| `--output` | `-o` | Output format (table/json/yaml) | `table` |
| `--verbose` | `-v` | Verbose output | `false` |
| `--app-manager` | | App Manager gRPC address | `unix:///var/run/aipc/app-manager.sock` |
| `--event-bus` | | Event Bus gRPC address | `unix:///var/run/aipc/event-bus.sock` |

`--api` is **not** a global flag: it is accepted per-command by the REST
subcommands only (media, process, logs, system, stream, files, event-log,
monitor). The global flags above are `--output/-o`, `--verbose/-v`,
`--app-manager`, and `--event-bus`.

---

## app — Application Management

```bash
aipc-cli app list                                    # List applications
aipc-cli app info <app-id>                           # Application details
aipc-cli app install <manifest> <image>              # Install application
aipc-cli app start <app-id>                          # Start
aipc-cli app stop <app-id>                           # Stop
aipc-cli app restart <app-id>                        # Restart
aipc-cli app remove <app-id>                         # Uninstall
aipc-cli app update <app-id> <manifest> <image>      # Update app (preserves data)
aipc-cli app dev <app-id>                            # Dev mode with hot reload
aipc-cli app stats <app-id>                          # Resource statistics
aipc-cli app logs <app-id> [-f] [--tail N]           # View logs
aipc-cli app exec <app-id> -- <command> [args...]    # Execute command inside container
```

**exec example**:
```bash
aipc-cli app exec myapp -- /bin/sh
aipc-cli app exec myapp -- ls -la /app
aipc-cli app exec myapp -u root -- cat /etc/os-release
```

**update example**:
```bash
aipc-cli app update my-app app.yaml new-image.tar   # Hot update, preserves volume data
```

**dev mode example**:
```bash
aipc-cli app dev my-app    # Bind-mount host source, auto-reload on file changes
```

---

## model — Model Management

```bash
aipc-cli model list                                  # List models
aipc-cli model info <model-id>                       # Model details
aipc-cli model register <model-path> [--id ID]       # Register model
aipc-cli model unregister <model-id>                 # Unregister model
aipc-cli model stats                                 # AI runtime statistics
```

---

## device — Device Control

```bash
aipc-cli device status                               # Device status
aipc-cli device light <level>                        # White light (0-100)
aipc-cli device ir <0-100>                          # IR LED level
aipc-cli device ircut <auto|day|night>               # IR-Cut mode
aipc-cli device ptz <action> [speed]                 # PTZ control
aipc-cli device zoom <in|out|stop> [speed]           # Zoom
aipc-cli device focus <near|far|auto|manual|stop>    # Focus
aipc-cli device gpio <read|write> <pin> [value]      # GPIO
```

**PTZ actions**: `left`, `right`, `up`, `down`, `stop`, `preset`, `save`

**Examples**:
```bash
aipc-cli device ptz left 50          # Pan left, speed 50
aipc-cli device ptz preset 1         # Call preset 1
aipc-cli device ptz save 1           # Save preset 1
aipc-cli device zoom in 30           # Zoom in, speed 30
aipc-cli device focus auto           # Auto focus
aipc-cli device gpio read 12         # Read GPIO 12
aipc-cli device gpio write 21 1      # GPIO 21 output high
```

---

## stream — Video Streams

```bash
aipc-cli stream list                                 # List streams
aipc-cli stream info <stream-id>                     # Stream details
aipc-cli stream url <stream-id> [--format rtsp|hls]  # Get stream URL
```

---

## event — Event Bus

```bash
aipc-cli event topics                                # List topics
aipc-cli event info <topic>                          # Topic details
aipc-cli event stats [topic]                         # Statistics
aipc-cli event publish <topic> <json> [--source S]   # Publish event
aipc-cli event subscribe <topic> [-f] [--raw]        # Subscribe to events
```

**Examples**:
```bash
aipc-cli event publish app/alert '{"msg":"hello"}'
aipc-cli event subscribe 'model/*/detections' -f
aipc-cli event subscribe app/test --raw --id my-sub
```

---

## media — Media Configuration

```bash
aipc-cli media config                                # View media configuration
aipc-cli media image [--brightness N] [--contrast N] # ISP parameters
aipc-cli media encoder --stream <name> [--bitrate N] # Encoding parameters
aipc-cli media rtsp --enable|--disable               # RTSP toggle
aipc-cli media ai-overlay --enable [--show-label]    # AI overlay
aipc-cli media osd <json-config>                     # OSD configuration
```

**encoder parameters**: `--stream` (main/sub/third), `--bitrate`, `--fps`, `--gop`

---

## system — System Management

```bash
aipc-cli system info                                 # System information
aipc-cli system stats                                # System statistics
aipc-cli system health                               # Health check
aipc-cli system status                               # Service status
aipc-cli system start                                # Start all services
aipc-cli system stop                                 # Stop all services
aipc-cli system restart                              # Restart all services
aipc-cli system enable                               # Enable auto-start on boot
aipc-cli system disable                              # Disable auto-start
```

---

## files — File Management

```bash
aipc-cli files list [path]                           # List files
aipc-cli files get <path>                            # Read file
aipc-cli files put <path> <content>                  # Write file
aipc-cli files upload <local> <remote>               # Upload file
aipc-cli files download <remote> [local]             # Download file
aipc-cli files delete <path>                         # Delete file
aipc-cli files mkdir <path>                          # Create directory
aipc-cli files rename <old> <new>                    # Rename
```

---

## logs — Log Viewer

```bash
aipc-cli logs services                               # List services
aipc-cli logs files                                  # List log files
aipc-cli logs show [service] [--lines N] [--level L] # View logs
aipc-cli logs download <file>                        # Download log
```

---

## monitor — Resource Monitoring

```bash
aipc-cli monitor summary                             # Resource overview
aipc-cli monitor cpu                                 # CPU usage
aipc-cli monitor memory                              # Memory usage
aipc-cli monitor disk                                # Disk usage
aipc-cli monitor network                             # Network statistics
```

---

## plugin — Plugin Management

```bash
aipc-cli plugin list                                 # List plugins
aipc-cli plugin info <app-id>                        # Plugin details
aipc-cli plugin capabilities                         # List capabilities
aipc-cli plugin check <app-id>                       # Check dependencies
```

---

## event-log — Event Log

```bash
aipc-cli event-log list [--category C] [--level L]   # List event logs
aipc-cli event-log stats                             # Statistics
aipc-cli event-log cleanup [--days N]                # Clean up old logs
```

---

## process — Process Management

```bash
aipc-cli process list [--sort cpu|mem|pid]           # List processes
aipc-cli process info <pid>                          # Process details
aipc-cli process kill <pid> [--signal SIGTERM]       # Terminate process
```

---

## completion — Shell Completion

```bash
source <(aipc-cli completion bash)                   # Bash
source <(aipc-cli completion zsh)                    # Zsh
```

---

## Environment Variables

| Variable | Description |
|----------|-------------|
| `AIPC_OUTPUT_FORMAT` | Output format (table/json/yaml) |
| `AIPC_VERBOSE` | Verbose output |

There is no `AIPC_API` environment variable; the REST API address is set per
command with `--api`.

## Configuration File

`~/.aipc/config.yaml`:

```yaml
grpc:
  app_manager: unix:///var/run/aipc/app-manager.sock
  ai_runtime: unix:///var/run/aipc/ai-runtime.sock
  event_bus: unix:///var/run/aipc/event-bus.sock
  device_control: unix:///var/run/aipc/device-control.sock
  timeout: 30s
output:
  format: table
  color: true
```
