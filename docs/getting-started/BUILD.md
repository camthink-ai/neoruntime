# NE503 Build Guide

## Quick Start

```bash
# Install build dependencies (Go, Node, pnpm, protoc)
./scripts/setup_env.sh layer1

# Build protobuf stubs + Go services + web console
make build-ci

# Build native platform services, HAL v2, C++ services, tools, and web console
make all
```

## Recommended Targets

| Target | What | Requirements |
| ------ | ---- | ------------ |
| `make build-go` | Protobuf stubs + Go platform services | Go, protoc |
| `make build-native` | `build-go` + HAL v2 + C++ services + tools | CMake, g++, gRPC C++ |
| `make build-web` | Web console | Node.js, pnpm |
| `make build-ci` | `build-go` + web console | Go, protoc, Node.js, pnpm |
| `make all` | `build-native` + web console | All local build dependencies |

## Environment Setup

### Automated (Ubuntu/macOS)

```bash
./scripts/setup_env.sh layer1    # Go + Node + pnpm + protoc
./scripts/setup_env.sh layer2    # + cmake + g++ + gRPC
./scripts/setup_env.sh layer3    # + Hailo SDK instructions
```

### Manual — Ubuntu 22.04

```bash
# Layer 1
sudo apt install -y golang-go nodejs protobuf-compiler
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
export PATH="$PATH:$(go env GOPATH)/bin"

# Layer 2
sudo apt install -y build-essential cmake protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev
```

### Manual — macOS

```bash
brew install go node protobuf cmake grpc
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
```

## Go and Web Build

No hardware dependencies. Works on any Linux/macOS.

| Tool | Min Version | Check |
|------|-------------|-------|
| Go | 1.25+ | `go version` |
| Node.js | 24+ | `node --version` |
| protoc | 3.15+ | `protoc --version` |
| protoc-gen-go | latest | `which protoc-gen-go` |
| protoc-gen-go-grpc | latest | `which protoc-gen-go-grpc` |
| Python | 3.8+ | `python3 --version` |

```bash
make build-ci
# equivalent to: make build-go build-web
```

Output binaries in `build/output/`:
- `device-control`, `event-bus`, `app-manager`, `platform-api`, `device-discovery`
- `web/dist/` (web assets)

All Go services compile with `CGO_ENABLED=0` (pure Go, no C dependencies).

## Native C/C++ Build

Adds HAL stub library and camera-daemon built for the host architecture.

| Tool | Min Version | Check |
|------|-------------|-------|
| CMake | 3.16+ | `cmake --version` |
| GCC/G++ | 10+ (C++20) | `g++ --version` |
| gRPC C++ | 1.30+ | `which grpc_cpp_plugin` |

```bash
make build-native
# equivalent to: make build-go hal-v2 camera-daemon ai-runtime aipc-cli tools
```

Additional output:
- `build/output/hal/stub/libaipc_hal*.so` (stub HAL)
- `build/output/camera-daemon` (native binary)

## Layer 3: Hailo-15 Cross-Compile

Requires Hailo Yocto Poky SDK for ARM cross-compilation.

### Prerequisites

- Hailo SDK 4.0.23 installed at `/opt/poky/4.0.23/`
- Available from Hailo developer portal

### Build Steps

```bash
# Source SDK environment (sets CC, CXX, CMAKE_TOOLCHAIN_FILE, etc.)
source /opt/poky/4.0.23/environment-setup-armv8a-poky-linux

# Verify cross-compiler
echo $CC   # should show: aarch64-poky-linux-gcc

# Build HAL v2 for Hailo-15
make hal-v2 HAL_PLATFORM=hailo15

# Cross-compile camera-daemon (uses cmake toolchain from SDK)
mkdir -p platform/camera-daemon/build && cd platform/camera-daemon/build
cmake -DCMAKE_TOOLCHAIN_FILE=$OECORE_TARGET_SYSROOT/../cmake/toolchain-aarch64-hailo.cmake ..
make -j$(nproc)
```

### Deploy to Device

```bash
# HAL libraries
scp build/output/hal/hailo15/lib*.so* root@192.0.2.72:/data/aipc/lib/hal/

# Platform services (Go ARM64 binaries)
scp build/output/device-control build/output/event-bus \
    build/output/app-manager build/output/platform-api \
    root@192.0.2.72:/data/aipc/bin/

# Camera daemon
scp build/output/camera-daemon root@192.0.2.72:/data/aipc/bin/
```

## Individual Targets

```bash
make proto                  # Generate Go protobuf code
make platform               # Build all Go services
make device-control         # Build device-control only
make hal-v2                  # Build HAL v2 (HAL_PLATFORM=stub, default)
make hal-v2 HAL_PLATFORM=hailo15 # Build HAL v2 for Hailo-15
make camera-daemon          # Build camera-daemon (native)
make aipc-cli               # Build CLI tool
make tools                  # Build shm-reader, nv12-to-jpeg
make web                    # Build web console
./scripts/install.sh         # Install to /data/aipc (no `make install` target)
make clean                  # Clean build artifacts
./scripts/setup_env.sh layer1  # Install/check build deps (no `make env-check` target)
make help                   # Show all targets
```

## Release Packaging

Build everything and produce a self-contained deployment tarball:

```bash
# Local stub release (for testing)
make pack
make pack VERSION=nx-1.0

# Hailo-15 full release (requires SDK)
make pack-release SDK_PATH=/opt/poky/4.0.23
make pack-release SDK_PATH=/opt/poky/4.0.23 VERSION=nx-1.0

# Legacy script (still works, delegates to Makefile)
./scripts/pack_release.sh --version nx-1.0
./scripts/pack_release.sh --sdk-path /opt/poky/4.0.23 --version nx-1.0
./scripts/pack_release.sh --skip-build --version nx-1.0   # repack only
```

Output: `build/release/aipc-<platform>-<version>.tar.gz`

### Tarball Contents

| Path | Contents |
|------|----------|
| `opt/aipc/bin/` | All binaries (services, CLI, tools) |
| `opt/aipc/lib/hal/` | HAL shared libraries |
| `opt/aipc/etc/` | Configuration files |
| `opt/aipc/web/` | Web console assets |
| `opt/aipc/models/` | HEF model files (if present) |
| `opt/aipc/swagger-ui/` | API documentation |
| `systemd/` | Systemd service units |
| `deploy.sh` | Hot-swap deployment script |
| `VERSION` | Version metadata |

### Deploy to Target

```bash
scp build/release/aipc-hailo15-nx-1.0.tar.gz root@192.0.2.72:/tmp/
ssh root@192.0.2.72
cd /tmp && tar xzf aipc-hailo15-nx-1.0.tar.gz
cd aipc-hailo15-nx-1.0 && ./deploy.sh

# Rollback
./deploy.sh --rollback
```

## CGo Status

All Go platform services build with `CGO_ENABLED=0` (pure Go, no C
dependencies):

| Service | CGo | Notes |
|---------|-----|-------|
| device-control | No | Pure Go; lens config is persisted via a JSON side-file |
| event-bus | No | Pure Go |
| platform-api | No | Pure Go |
| app-manager | No | Pure Go |

HAL `.so` loading (dlopen/dlsym) is implemented in C++, not Go — see
`platform/ai-runtime/src/hal_ml_loader.cpp` and
`platform/camera-daemon/include/hal_loader.h`.

## FAQ

### `protoc: not found`

```bash
sudo apt install protobuf-compiler
```

### `protoc-gen-go: not found`

```bash
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest
export PATH="$PATH:$(go env GOPATH)/bin"
```

### `grpc_cpp_plugin: not found` (camera-daemon build)

```bash
sudo apt install protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev
```

### camera-daemon cmake picks up wrong toolchain

```bash
rm -rf platform/camera-daemon/build
mkdir platform/camera-daemon/build && cd platform/camera-daemon/build
cmake ..  # fresh configure
```

### HAL v2 hailo15 build fails — SDK not found

```bash
source /opt/poky/4.0.23/environment-setup-armv8a-poky-linux
make hal-v2 HAL_PLATFORM=hailo15
```
