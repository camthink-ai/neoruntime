# Cross-Platform Deployment Guide

## Overview

The `build/output/` directory contains compiled binaries that can be deployed directly to the target platform. Before deployment, ensure the target platform architecture matches the build artifacts.

## Current Build Artifacts

```bash
build/output/
├── ai-runtime          # AI inference service (C++)
├── app-manager         # Application management service (Go)
├── camera-daemon       # Media pipeline service (C++)
├── device-control      # Device control service (Go)
├── device-discovery    # Device discovery service (Go)
├── event-bus           # Event bus service (Go)
├── platform-api        # Platform API gateway (Go)
├── aipc-cli            # Command-line tool (Go)
├── aipc-os-updater     # OS updater (Go)
├── nv12-to-jpeg        # Tool (C++)
├── shm-reader          # Tool (C++)
├── mcu-firmware        # MCU firmware package
├── hal/
│   └── <platform>/     # HAL v2 libraries (C++)
│       ├── libaipc_hal*.so
│       └── libhal-*.so
└── web/                # Web console assets
```

## Architecture Check

### Check Current Build Artifact Architecture

```bash
# Check binary architecture
file build/output/ai-runtime

# Example output:
# ELF 64-bit LSB executable, x86-64  # x86_64 architecture
# ELF 64-bit LSB executable, ARM aarch64  # ARM64 architecture
```

### Check Target Platform Architecture

```bash
# Execute on target platform
uname -m
# Output: x86_64 or aarch64 or armv7l
```

**Important:** The build artifact architecture must match the target platform architecture.

## Cross-Platform Build

### Go Service Cross-Compilation

Go services support cross-compilation without building on the target platform:

```bash
# ARM64 (common embedded platform)
export GOOS=linux
export GOARCH=arm64
make platform

# ARMv7 (32-bit ARM)
export GOOS=linux
export GOARCH=arm
export GOARM=7
make platform

# x86_64 (default)
export GOOS=linux
export GOARCH=amd64
make platform
```

**Note:** `make platform` builds the Go services only (`device-control`,
`event-bus`, `app-manager`, `platform-api`, `device-discovery`,
`aipc-os-updater`). The C++ binaries `ai-runtime` and `camera-daemon` are built
by `make ai-runtime` / `make camera-daemon` (or `make build-native`), not by
`make platform`.

### C++ Component Cross-Compilation

C++ components (camera-daemon, HAL libraries) require a cross-compilation toolchain:

```bash
# Install cross-compilation toolchain (ARM64 example)
sudo apt-get install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu

# Cross-compile camera-daemon
cd platform/camera-daemon
mkdir -p build && cd build
cmake .. \
  -DCMAKE_SYSTEM_NAME=Linux \
  -DCMAKE_SYSTEM_PROCESSOR=aarch64 \
  -DCMAKE_C_COMPILER=aarch64-linux-gnu-gcc \
  -DCMAKE_CXX_COMPILER=aarch64-linux-gnu-g++
make -j$(nproc)
```

The repository's canonical build path is `make camera-daemon` (with `SDK_PATH`
set for Hailo-15); the inline CMake above is only a general example.

## Deployment Methods

### Method 1: Using Deployment Script (Recommended)

Two distinct scripts are provided:

**On-device hot-swap** — `deploy.sh` runs **on the target device** (requires
root) and hot-swaps the AIPC release under `/data/aipc`, with automatic backup
and rollback. It takes no IP argument:

```bash
# Package the release on the build machine
make pack VERSION=nx-1.0

# Copy the tarball to the device, then run deploy.sh ON the device
scp build/release/aipc-*-nx-1.0.tar.gz root@<device-ip>:/tmp/
ssh root@<device-ip> 'cd /tmp && tar xzf aipc-*-nx-1.0.tar.gz && cd aipc-*-nx-1.0 && ./deploy.sh'
```

Options: `--no-config` (keep existing config), `--rollback`, `--status`,
`--force`.

**SSH push deploy** — `scripts/quick-deploy.sh <target-host> [username]`
pushes built artifacts over SSH:

```bash
./scripts/quick-deploy.sh 192.168.1.100 root
```

It auto-detects the target architecture, transfers binaries and configs, and
installs the systemd services.

### Method 2: Manual Deployment

#### Step 1: Prepare Deployment Package

```bash
# Create deployment directory
mkdir -p deploy/aipc/{bin,lib/hal,etc,logs}

# Copy binary files
cp build/output/* deploy/aipc/bin/
find build/output/hal -type f \( -name "libaipc_hal*.so*" -o -name "libhal-*.so*" \) \
  -exec cp -aP {} deploy/aipc/lib/hal/ \; 2>/dev/null || true

# Copy configuration files
# (on-device configs are flat: configs/{platform,ai}/*.yaml are flattened to
#  etc/*.yaml by the Makefile; services read them from etc/ directly)
cp -r configs/* deploy/aipc/etc/

# Package
cd deploy
tar czf aipc-platform.tar.gz aipc/
```

#### Step 2: Transfer to Target Platform

```bash
# Using scp
scp deploy/aipc-platform.tar.gz user@target:/tmp/

# Or using rsync (incremental sync)
rsync -avz build/output/ user@target:/data/aipc/bin/
```

#### Step 3: Install on Target Platform

```bash
# SSH to target platform
ssh user@target

# Extract
cd /tmp
tar xzf aipc-platform.tar.gz -C /data/

# Set permissions
chmod +x /data/aipc/bin/*
chmod 644 /data/aipc/etc/*.yaml

# Create runtime directories
mkdir -p /run/aipc/{shm,sockets}
mkdir -p /data/aipc/logs
```

### Method 3: Using Docker (Build Environment Only)

There is no `Dockerfile.deploy` image in this repository. Docker is used for
the cross-build **environment**, not for deploying the runtime:

```bash
docker/dev/build.sh your-org/ne503-dev-env:v1.0          # lightweight dev image
docker/dev/build-full.sh /opt/poky/4.0.23 4.0.23 your-org/ne503-dev-env-full
make docker-dev                                         # enter persistent dev container
```

Inside the container, build and package with `make pack-release VERSION=...`;
the tarball is then deployed to the device (Method 1).

## Dependency Check

### Runtime Dependencies

The target platform requires the following dependencies:

#### Binary Dependencies

```bash
# Check dynamic libraries (C++ services such as ai-runtime/camera-daemon
# link against libstdc++, gRPC, and the HAL library; Go services are static)
ldd build/output/ai-runtime

# Go services are built with CGO_ENABLED=0 (pure Go, no dynamic deps)
```

**Static Compilation (used by the project):**

All Go platform services build with `CGO_ENABLED=0` (pure Go, no C
dependencies), so no `-static` linker flags are needed. See `Makefile`
(`GO_BUILD_FLAGS ?= -v -mod=mod`, `CGO_ENABLED=0`).

```bash
make platform   # Go services, CGO_ENABLED=0
```

#### C++ Binary Dependencies

```bash
# Check dependencies
ldd build/output/camera-daemon

# May need:
# - libstdc++.so.6
# - libgcc_s.so.1
# - libc.so.6
```

### System Service Dependencies

```bash
# containerd (required by app-manager)
systemctl status containerd

# If not installed
# Ubuntu/Debian:
sudo apt-get install containerd

# Or use the project-provided containerd configuration
```

## Configuration File Adaptation

When deploying to different platforms, configuration files need to be modified:

### 1. Network Configuration

```yaml
# configs/platform-api.yaml
service:
  listen: "0.0.0.0:8080"  # Adjust based on target platform network
```

### 2. Path Configuration

```yaml
# configs/platform/app-manager.yaml
apps:
  registry_path: /data/aipc/apps/registry
  instances_path: /data/aipc/apps/instances
  manifests_path: /etc/aipc/apps
```

### 3. Socket Paths

```yaml
# Ensure socket directory exists and has write permissions
service:
  listen: unix:///run/aipc/app-manager.sock
```

## Verify Deployment

### 1. Check Binary Files

```bash
# Execute on target platform
file /data/aipc/bin/ai-runtime
ldd /data/aipc/bin/ai-runtime
```

### 2. Test Service Startup

```bash
# Manual startup test
/data/aipc/bin/ai-runtime -config /data/aipc/etc/ai-runtime.yaml

# Check logs
tail -f /data/logs/ai-runtime.log
```

### 3. Check Service Status

```bash
# If using systemd
systemctl status ai-runtime
systemctl status aipc-platform.target

# View AIPC platform services (aipc-* units are the platform meta-units;
# core services are ai-runtime, app-manager, camera-daemon, device-control,
# device-discovery, event-bus, platform-api)
systemctl list-units 'aipc-*'
```

## Common Issues

### Issue 1: "exec format error"

**Cause:** Architecture mismatch

**Solution:** Re-cross-compile to match target architecture

```bash
export GOOS=linux GOARCH=arm64
make platform
```

### Issue 2: "No such file or directory"

**Cause:** Missing dynamic libraries

**Solution:**
- Use static compilation, or
- Install missing libraries on the target platform

```bash
# Check missing libraries
ldd /data/aipc/bin/ai-runtime | grep "not found"
```

### Issue 3: "Permission denied"

**Cause:** File permission issue

**Solution:**
```bash
chmod +x /data/aipc/bin/*
```

### Issue 4: Socket Creation Failure

**Cause:** Directory does not exist or insufficient permissions

**Solution:**
```bash
mkdir -p /run/aipc/sockets
chmod 777 /run/aipc/sockets  # Or use appropriate permissions
```

## Quick Deployment Checklist

- [ ] Check target platform architecture (`uname -m`)
- [ ] Cross-compile binaries matching the architecture
- [ ] Check runtime dependencies (`ldd`)
- [ ] Prepare configuration files and adapt paths
- [ ] Create necessary directory structure
- [ ] Set correct file permissions
- [ ] Test service startup
- [ ] Configure systemd services (if needed)

## Automated Deployment Script Example

```bash
#!/bin/bash
# deploy-to-target.sh

TARGET=$1
ARCH=$(ssh $TARGET "uname -m")

echo "Target architecture: $ARCH"

# Cross-compile
export GOOS=linux
case $ARCH in
  aarch64) export GOARCH=arm64 ;;
  armv7l) export GOARCH=arm GOARM=7 ;;
  x86_64) export GOARCH=amd64 ;;
esac

make clean
make platform

# Deploy
./scripts/quick-deploy.sh $TARGET
```

Usage:
```bash
./deploy-to-target.sh user@192.168.1.100
```
