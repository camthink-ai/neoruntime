# MVP (Minimum Viable Product) Guide

## Overview

The MVP version of AIPC Platform includes all core services running with HAL stubs, allowing full testing and demonstration without hardware.

## Quick Start

### 1. Build All Components

```bash
# Check build environment
./scripts/check_build.sh

# Build everything
make all
```

### 2. Start MVP Services

```bash
# Start all core services
./scripts/start_mvp.sh
```

This will start:
- **Event Bus** - Message pub/sub service
- **Device Control** - Device/MCU control service
- **AI Runtime** - AI inference service
- **App Manager** - Application lifecycle management
- **Platform API** - REST/WebSocket gateway (binds `127.0.0.1:8080`)

### 3. Test MVP

```bash
# Test service availability
./scripts/test_mvp.sh
```

### 4. Stop Services

```bash
# Stop all services
./scripts/stop_mvp.sh
```

## Service Endpoints

All services use Unix domain sockets:

- **Event Bus**: `unix:///run/aipc/event-bus.sock`
- **Device Control**: `unix:///run/aipc/device-control.sock`
- **AI Runtime**: `unix:///run/aipc/ai-runtime.sock`
- **App Manager**: `unix:///run/aipc/app-manager.sock`

## Logs

Service logs are written to `/tmp/`:

- `/tmp/aipc-event-bus.log`
- `/tmp/aipc-device-control.log`
- `/tmp/aipc-ai-runtime.log`
- `/tmp/aipc-app-manager.log`

## Using Python SDK

```python
from hailo_ipc_sdk import InferenceClient, DeviceClient, EventClient

# Connect to AI Runtime
inference = InferenceClient()
models = inference.list_models()
print(f"Available models: {models}")

# Connect to Device Control
device = DeviceClient()
status = device.get_device_status()
print(f"Device status: {status}")

# Connect to Event Bus
events = EventClient()
events.publish("test/topic", {"message": "Hello MVP"})
```

## MVP Features

### ✅ Working Features

1. **Service Communication**
   - All services can start and communicate via gRPC
   - Unix domain sockets for local communication
   - Service health checking

2. **AI Runtime**
   - Model registration (with HAL stub)
   - Model listing
   - Inference requests (simulated)
   - Stream inference (with SHM fallback)

3. **Event Bus**
   - Publish/subscribe events
   - Topic management
   - Event streaming

4. **Device Control**
   - Device status query
   - Light control (simulated)
   - GPIO control (simulated)

5. **App Manager**
   - Application registry
   - Manifest parsing
   - Security sandbox configuration

### ⚠️ Limitations (MVP)

1. **HAL Stubs Only**
   - No real hardware access
   - Simulated responses
   - No actual video capture
   - No real AI inference

2. **No Camera Daemon**
   - Camera daemon not included in MVP startup
   - SHM streams not available by default

3. **No Containerd Integration**
   - App Manager cannot actually run containers
   - Application lifecycle is simulated

## Next Steps

After MVP validation:

1. **Phase 2**: Implement real HAL for Hailo-15
2. **Phase 3**: Integrate GStreamer for camera
3. **Phase 4**: Add containerd integration
4. **Phase 5**: Web console development

## Troubleshooting

### Services Won't Start

1. Check if binaries exist:
   ```bash
   ls -la build/output/
   ```

2. Check if HAL v2 stub is available:
   ```bash
   ls -la build/output/hal/stub/libaipc_hal.so
   ```

3. Check logs:
   ```bash
   tail -f /tmp/aipc-*.log
   ```

### Permission Errors

```bash
# Create runtime directories with proper permissions
sudo mkdir -p /run/aipc
sudo mkdir -p /run/aipc/shm
sudo chmod 777 /run/aipc
sudo chmod 777 /run/aipc/shm
```

### Port/Socket Conflicts

```bash
# Stop all services first
./scripts/stop_mvp.sh

# Remove old sockets
rm -f /run/aipc/*.sock

# Restart
./scripts/start_mvp.sh
```

## Development

### Adding New Services

1. Build service binary to `build/output/`
2. Add service to `start_mvp.sh`:
   ```bash
   nohup "$BUILD_DIR/new-service" \
       -config configs/platform/new-service.yaml \
       > /tmp/aipc-new-service.log 2>&1 &
   ```
3. Add to `stop_mvp.sh` services list
4. Add to `test_mvp.sh` service checks

### Testing New Features

1. Start MVP: `./scripts/start_mvp.sh`
2. Test manually or with Python SDK
3. Check logs for errors
4. Update test script if needed

## Support

For issues or questions:
- Check logs: `/tmp/aipc-*.log`
- Review documentation: `docs/`
- Check build status: `./scripts/check_build.sh`
