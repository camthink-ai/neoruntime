# AIPC Platform - Quick Start Guide

Get up and running with the AIPC Platform in minutes.

---

## Prerequisites

- **Go** 1.25+ ([Install](https://golang.org/dl/))
- **CMake** 3.16+ ([Install](https://cmake.org/download/))
- **protoc** 3.15+ ([Install](https://grpc.io/docs/protoc-installation/))
- **Python 3** 3.8+
- **Node.js** 24+ (for web console)
- **GCC/G++** 10+ (for C++ components)

**Windows users:** See [WINDOWS_SETUP.md](WINDOWS_SETUP.md) for detailed instructions.

---

## Quick Setup

### 1. Clone and Enter Project

```bash
cd /path/to/ne503
```

### 2. Build Everything

```bash
# Build protobuf stubs + Go services + web console
make build-ci

# Or build individual components
make proto          # Generate protobuf code
make platform       # Build all Go services
make web            # Build web console
```

### 3. Verify Build

```bash
./scripts/check_build.sh
ls -lh build/output/
```

You should see:
- `device-control`, `event-bus`, `app-manager`, `platform-api`
- `web/dist/` (web assets)

### 4. Build C++ Components (Optional)

```bash
# Native platform build: adds HAL stub, C++ services, CLI, and tools
make build-native
```

---

## Deploy to Device

```bash
# Build release package
make pack VERSION=nx-1.0

# Copy the tarball to the device, then run the hot-swap script ON the device
# (deploy.sh runs on-device and takes options such as --rollback/--status, not an IP)
scp build/release/aipc-*-nx-1.0.tar.gz root@<device-ip>:/tmp/
ssh root@<device-ip> 'cd /tmp && tar xzf aipc-*-nx-1.0.tar.gz && cd aipc-*-nx-1.0 && ./deploy.sh'
```

---

## Start Services

```bash
# Using MVP scripts
./scripts/start_mvp.sh

# On device with systemd
sudo systemctl start aipc-platform

# Verify
sudo systemctl status aipc-platform
# Web Console: http://<device-ip>/  (nginx gateway on :80/:443; platform-api itself binds 127.0.0.1:8080)
```

---

## Development Workflow

### Build Commands

```bash
make all              # Build native platform artifacts + web console
make build-go         # Protobuf stubs + Go services
make build-native     # Go services + HAL v2 + C++ services + tools
make build-web        # Web console only
make build-ci         # Go services + web console
make proto            # Compile .proto files
make platform         # Build all Go services
make hal-v2           # Build HAL v2 (default: stub)
make hal-v2 HAL_PLATFORM=hailo15  # Build for Hailo-15
make camera-daemon    # Build camera-daemon
make web              # Build web console
make clean            # Clean build artifacts
```

### Test Commands

```bash
make test                        # Go unit tests
make test-basic                  # Read-only repository checks
make test-smoke                  # HTTP smoke tests against running services
make test-verify                 # Basic checks + unit tests
./scripts/test_mvp.sh            # MVP service checks
go test ./platform/...           # Go tests only
```

---

## Troubleshooting

### "protoc: not found"

```bash
sudo apt install protobuf-compiler
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
export PATH="$PATH:$(go env GOPATH)/bin"
```

### Build Fails

```bash
make clean && make build-ci
```

### Services Won't Start

```bash
ls -l /run/aipc/          # Check runtime directories
ls -l /data/aipc/lib/hal/  # Check HAL library
journalctl -u "aipc-*" -f # Check logs
```

---

## Next Steps

1. [Build Guide](BUILD.md) - Detailed build instructions
2. [Architecture](../architecture/README.md) - System architecture
3. [HAL v2 API Reference](../references/hal-v2-api-reference.md) - Hardware abstraction interfaces
4. SDK and app development guides live in `camthink-ai/ne503-aipc-sdks` and `camthink-ai/ne503-aipc-apps`
