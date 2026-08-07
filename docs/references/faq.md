# AIPC Platform FAQ

## Build Issues

### Q: make build-ci fails with protobuf not found?

**A:** Make sure the Protocol Buffers compiler is installed. Run `sudo apt install protobuf-compiler`, then re-run `make build-ci`.

### Q: How to compile HAL v2 stub mode?

**A:** Use `make hal-v2 HAL_PLATFORM=stub` to compile the HAL v2 stub mode. This generates `build/output/hal/stub/libaipc_hal.so`, which allows testing platform functionality without real hardware.

```bash
make hal-v2 HAL_PLATFORM=stub
make all  # Rebuild all services that depend on HAL v2
```

### Q: C++ compilation fails with missing header files?

**A:** Make sure CMake is configured correctly. Check the path settings in `platform/*/CMakeLists.txt`, or clean and rebuild with `make clean`.

```bash
make clean
make all
```

## Deployment Issues

### Q: How to flash firmware to the device?

**A:** Use the firmware tool provided by Hailo. Place the firmware file in `/opt/firmware/`, then run:

```bash
sudo hailo-update -f /opt/firmware/hailo15_fw.bin
reboot  # Reboot the device for firmware to take effect
```

### Q: What is the service startup order?

**A:** Service dependencies are as follows. systemd will automatically start them in this order:

```
containerd → camera-daemon → ai-runtime → event-bus → platform-api, device-control, app-manager
```

Use `aipc-cli system start` to start all services in the correct order.

### Q: How to view service logs?

**A:** Use journalctl to view system logs:

```bash
# View live logs
journalctl -u aipc-ai-runtime -f

# View logs from the last hour
journalctl -u aipc-camera-daemon --since "1 hour ago"

# View error logs
journalctl -u aipc-platform-api -p err
```

## Inference Issues

### Q: What model formats are supported?

**A:** The platform primarily supports Hailo Optimized Format (.hef) models, compatible with the following formats:

- YOLOv8/YOLOv5 (.pt conversion)
- TensorFlow Lite (.tflite)
- ONNX (.onnx, requires conversion to .hef)
- PyTorch (.pth, requires conversion to .hef)

### Q: How to adjust inference concurrency?

**A:** Modify the scheduler configuration in `configs/ai/ai-runtime.yaml`:

```yaml
scheduler:
  global_qps_limit: 100        # Global QPS limit
  global_concurrent_limit: 16  # Global concurrency limit
  default_session:
    max_qps: 30                 # QPS per session
    max_concurrent: 2           # Concurrency per session
```

Restart the ai-runtime service after making changes.

### Q: What to do if NPU temperature is too high?

**A:** Throttling activates automatically when temperature exceeds 80°C, and a warning is issued above 85°C. Solutions:

1. Check the cooling system: clean fans, improve ventilation
2. Reduce inference load: decrease concurrent sessions or lower FPS
3. Use temperature protection: set temperature limits in configuration

```yaml
monitoring:
  temperature_limit_c: 85
  throttle_temperature_c: 80
```

## Streaming Issues

### Q: Does the platform support RTSP pull?

**A:** RTSP 1.0 protocol pulling is supported, but the platform primarily serves as an RTSP server for pushing streams. For pulling, FFmpeg is recommended:

```bash
# Pull RTSP stream
ffmpeg -rtsp_transport tcp -i rtsp://camera-ip:554/stream -c copy output.mp4
```

### Q: How to optimize high frontend playback latency?

**A:** Optimization methods:

1. Adjust encoding parameters: reduce GOP size, increase frame rate
2. Enable hardware acceleration: use WebCodecs instead of MSE
3. Optimize network: ensure sufficient LAN bandwidth
4. Reduce buffering: lower WebSocket buffer size

```yaml
encoder:
  gop: 15        # Reduce GOP to 15 frames (0.5 seconds)
  fps: 30        # Increase frame rate
  profile: high  # Use high profile
```

### Q: Does the platform support multiple video streams?

**A:** Yes, by configuring multiple encoder instances:

```yaml
encoder:
  main:
    width: 1920
    height: 1080
    fps: 30
  sub:
    width: 1280
    height: 720
    fps: 25
```

Each stream is managed independently with different resolutions and frame rates.

## Container Issues

### Q: How do applications access platform services?

**A:** Main containers automatically receive platform socket access via environment variables:

```bash
# Connect to AI Runtime inside the container
export AI_RUNTIME_ENDPOINT=/run/aipc/ai-runtime.sock

# Connect to event bus
export EVENT_BUS_ENDPOINT=/run/aipc/event-bus.sock
```

### Q: How do containers communicate with each other?

**A:**

1. **Main/Sub containers**: Communicate via shared volumes and networking
2. **Cross-application**: Via Event Bus publish/subscribe
3. **Direct access**: Main containers can access other applications' Main containers

```yaml
# Main container configuration
volumes:
  - name: shared-data
    host: /opt/aipc/data/shared
    container: /app/data

# Network configuration
networking:
  mode: internal
  ingress:
    - port: 80
      target: api-gateway:8080
```

### Q: How to limit container resources?

**A:** Configure resource limits in the application manifest:

```yaml
resources:
  cpu: "1.0"          # 1 CPU core
  memory: "512Mi"     # 512MB memory
  pids_limit: 100      # Maximum process count

# Or set defaults in global configuration
default_cpu_quota: 50
default_memory_mb: 256
```

## SDK Issues

### Q: How to install the Python SDK?

**A:** Install the development version using pip:

```bash
# Clone the SDK repository
git clone git@github.com:camthink-ai/ne503-aipc-sdks.git
cd ne503-aipc-sdks/python

# Install SDK
pip install -e .

# Verify installation
python -c "from hailo_ipc_sdk import InferenceClient; print('SDK installed')"
```

### Q: How to use the SDK inside a container?

**A:** Add SDK installation in the Dockerfile:

```dockerfile
FROM python:3.9-slim

# Install SDK
RUN pip install hailo-ipc-sdk

# Set environment variables
ENV AI_RUNTIME_ENDPOINT=/run/aipc/ai-runtime.sock
ENV EVENT_BUS_ENDPOINT=/run/aipc/event-bus.sock

# Application code
COPY . /app
CMD ["python", "app.py"]
```

### Q: Which languages are supported?

**A:** The following language SDKs are currently supported:

- **Python**: Primary SDK, supports all features
- **Go**: Basic gRPC support
- **C++**: Performance-sensitive scenarios
- **TypeScript/JavaScript**: Frontend integration

Future plans include Rust and Java support.

## Performance Issues

### Q: How to optimize inference performance?

**A:** Optimization suggestions:

1. Use batch inference
2. Adjust model precision (FP16/INT8)
3. Optimize input preprocessing
4. Use an appropriate scheduling strategy

```yaml
performance:
  device_mode: high    # High-performance mode
  batch_enabled: true  # Enable batching
  batch_size: 4       # Batch size
```

### Q: How to monitor system performance?

**A:** Use the built-in monitoring tools:

```bash
# View AI Runtime statistics
aipc-cli ai-runtime stats

# View container resource usage
aipc-cli app-manager get-app-stats <app-id>

# View NPU performance
hailo-smi

# Real-time monitoring
aipc-cli system health
```

### Q: What to do about high memory usage?

**A:** Solutions:

1. Check model cache count
2. Limit concurrent sessions
3. Optimize application memory usage
4. Increase system memory

```yaml
performance:
  memory_limit_mb: 2048
  max_model_cache: 2  # Reduce model cache count
```

## Security Issues

### Q: How to enhance platform security?

**A:** Security hardening measures:

1. Enable read-only filesystem
2. Restrict container capabilities
3. Use Seccomp profiles
4. Update dependencies regularly

```yaml
security:
  readonly_rootfs: true
  no_new_privileges: true
  seccomp_profile: /etc/aipc/seccomp-default.json
```

### Q: How are container permissions controlled?

**A:** Permissions are controlled via capabilities:

```yaml
security:
  dropped_capabilities:
    - CAP_SYS_ADMIN
    - CAP_NET_ADMIN
    - CAP_SYS_MODULE
```

Main containers receive necessary permissions; Sub containers follow the principle of least privilege.

### Q: How to handle security vulnerabilities?

**A:**

1. Run security scans regularly: `gosec ./...`
2. Update dependencies: `go get -u`
3. Apply security patches
4. Monitor vulnerability reports

## Development Issues

### Q: How to develop support for a new model?

**A:** Development steps:

1. Add a new post-processing type in HAL
2. Update the model registration API
3. Implement the corresponding post-processing logic
4. Add test cases

```cpp
// Add a new post-processing type in HAL
case HAL_POST_TYPE_CUSTOM:
    // Custom post-processing logic
    break;
```

### Q: How to contribute code?

**A:** Contribution process:

1. Fork the project
2. Create a feature branch
3. Write tests
4. Submit a PR
5. Code review

Ensure all code passes `make test` and `make lint` checks.

### Q: How to debug service issues?

**A:** Debugging methods:

1. Enable debug logging
2. Test APIs with grpcurl
3. Check socket connections
4. View detailed error messages

```bash
# Enable debug logging
export LOG_LEVEL=debug

# Test API connection
grpcurl -plaintext -d '{}' unix:///run/aipc/service.sock list
```

## Troubleshooting

### Q: What to do if a service fails to start?

**A:** Troubleshooting steps:

1. Check if dependency services are running
2. View error logs
3. Verify configuration files
4. Check system resources

```bash
systemctl status aipc-*.service
journalctl -u aipc-service -f
```

### Q: What to do if model registration fails?

**A:** Solutions:

1. Check model path
2. Verify model format
3. Check NPU status
4. Verify permission settings

```bash
hailo-smi
ls -la /opt/aipc/models/
```

### Q: Container application cannot access external network?

**A:** Network configuration:

```yaml
networking:
  mode: bridge  # Use bridge mode
  port_mappings:
    - container: 80
      host: 8080
```

Or add network parameters at runtime:

```bash
aipc-cli app-manager start-app <app-id> --network=host
```

## Common Error Codes

| Error Code | Description | Solution |
|------------|-------------|----------|
| E001 | Service not started | Check service status |
| E002 | Socket connection failed | Check socket file |
| E003 | Model load failed | Verify model file |
| E004 | Session quota exceeded | Adjust configuration |
| E005 | Insufficient permissions | Check user permissions |
| E006 | Out of memory | Increase memory or optimize |

## Support

If you encounter issues, you can get support through the following channels:

1. Read the documentation: `docs/`
2. Check logs: `journalctl -u aipc-*`
3. Run diagnostics: `make test-smoke`
4. Submit an issue: GitHub Issues

Make sure you have read this document and tried basic troubleshooting steps before submitting an issue.
