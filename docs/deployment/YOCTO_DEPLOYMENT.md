# Yocto Deployment Guide

This document describes how to build and deploy the AIPC Platform on a Hailo-15
Yocto image.

Two deployment models exist:

- **On-device release (current, recommended)** — the AIPC release tree lives on
  the persistent `/data/aipc` partition and is updated with the bundled
  `deploy.sh` hot-swap script. The Yocto image only needs the OS-owned
  `aipc-bootstrap` package (from the `meta-hailo-camthink` layer).
- **Rootfs pre-seed (legacy)** — copy the release into `/usr/bin`, `/etc/aipc`
  and `/etc/systemd/system` while baking the image with
  `scripts/install_to_yocto.sh`.

## Quick Start

### 1. Build the release package

```bash
# Full Hailo-15 cross-compiled release (requires the Yocto SDK at /opt/hailo-sdk)
make pack-release VERSION=nx-1.0

# Artifact
ls -la build/release/
# -> build/release/aipc-hailo15-nx-1.0.tar.gz
```

`pack-release` auto-detects the SDK environment script
(`<SDK_PATH>/environment-setup-*-poky-linux`) and cross-compiles the Go
services, HAL v2 and camera-daemon with `HAL_PLATFORM=hailo15`. Point at a
non-default SDK with `SDK_PATH=/path/to/poky-sdk make pack-release ...`.

### 2. Install into a Yocto rootfs

```bash
# Method 1: install script (consumes the staged release tree)
./scripts/install_to_yocto.sh /path/to/yocto/rootfs

# Method 2: manual, from the tarball
mkdir -p /tmp/aipc && tar -xzf build/release/aipc-hailo15-nx-1.0.tar.gz -C /tmp/aipc
cp -r /tmp/aipc/aipc-hailo15-nx-1.0/opt/aipc/* /path/to/yocto/rootfs/opt/aipc/
cp /tmp/aipc/aipc-hailo15-nx-1.0/systemd/*.service /path/to/yocto/rootfs/etc/systemd/system/
```

### 3. Yocto recipe integration (production)

The OS image does not bundle the AIPC release tree. It ships the
`aipc-bootstrap` package from the `meta-hailo-camthink` layer (launchers,
systemd units, sysctl/journald drop-ins, `/etc/aipc-os-release`); the AIPC
runtime is installed onto the persistent `/data` partition at runtime. See
[`./os-upgrade.md`](./os-upgrade.md) for the required image contents.

## Build Process

### Release package layout

`make pack`/`make pack-release` stage a self-contained tree under
`build/release/` and pack it into `aipc-hailo15-<version>.tar.gz`:

```text
build/release/
└── aipc-hailo15-<version>/
    ├── deploy.sh                     # on-device hot-swap deployer
    ├── VERSION
    └── opt/aipc/
        ├── bin/                      # aipc-os-updater, aipc-cli, helper scripts
        ├── libexec/                  # aipc-restore, aipc-firstboot, ...
        ├── lib/hal/                  # libaipc_hal*.so (Hailo-15 HAL)
        ├── etc/                      # *.yaml, security/, systemd drop-ins
        ├── etc/security/             # seccomp-default.json, event-acl.yaml
        ├── etc/swagger.yaml
        ├── nginx/                    # app gateway config + nginx runtime
        ├── scripts/                  # aipc-install-current-root.sh, ...
        ├── firmware/mcu/             # ne503_ota_package_*.bin
        ├── docs/
        ├── web/                      # web console
        ├── swagger-ui/
        ├── models/                   # detection/ classification/ ... genai/
        └── app-manifest.json
    └── systemd/                      # *.service, *.timer, *.target units
```

### Makefile targets

| Target | HAL | SDK required | Output |
|--------|-----|--------------|--------|
| `make pack-release VERSION=<v>` | `hailo15` | Yes (`SDK_PATH`, default `/opt/hailo-sdk`) | `build/release/aipc-hailo15-<v>.tar.gz` (device) |
| `make pack VERSION=<v>` | `stub` (host arch) | No | `build/release/aipc-stub-<v>.tar.gz` (dev host) |
| `make docker-pack-release VERSION=<v>` | Docker multi-arch image | via Docker | container image |
| `make all` + `make hal-v2 HAL_PLATFORM=hailo15` | `hailo15` | Yes | build outputs (dev, see `BUILD.md`) |

### Cross-compilation requirements

Go services cross-compile with `GOOS=linux GOARCH=arm64` automatically. The
C/C++ components (HAL v2, camera-daemon) need the **Yocto Poky SDK**, not the
Ubuntu distro cross-compiler (`gcc-aarch64-linux-gnu` cannot build against the
Hailo sysroot):

```bash
# Install the SDK from the Hailo developer portal, then source its environment:
source /opt/poky/4.0.23/environment-setup-armv8a-poky-linux
make hal-v2 HAL_PLATFORM=hailo15
```

See [`../getting-started/BUILD.md`](../getting-started/BUILD.md) for the full
layer-3 SDK setup.

## Deploy to Device

### Method 1: On-device hot-swap release (recommended)

The tarball bundles `deploy.sh`, which stops the AIPC services, atomically
swaps the release under `/data/aipc` (with automatic backup and rollback), and
restarts the services.

```bash
# 1. Build
make pack-release VERSION=nx-1.0

# 2. Transfer to the device
scp build/release/aipc-hailo15-nx-1.0.tar.gz device:/tmp/

# 3. Extract and deploy on the device
ssh device "cd /tmp && tar -xzf aipc-hailo15-nx-1.0.tar.gz && cd aipc-hailo15-nx-1.0 && sudo ./deploy.sh"

# 4. Check status
ssh device "sudo /tmp/aipc-hailo15-nx-1.0/deploy.sh --status"
```

`deploy.sh` validates package integrity before switching and refuses to run if
the release YAMLs still contain non-canonical `/opt/aipc` paths.

### Method 2: Rootfs pre-seed (legacy)

```bash
# 1. Build
make pack-release VERSION=nx-1.0

# 2. Install into the rootfs being baked by Yocto
./scripts/install_to_yocto.sh /path/to/rootfs

# 3. Enable services in the image
systemctl enable ai-runtime.service
systemctl enable app-manager.service
systemctl enable device-control.service
systemctl enable event-bus.service
systemctl enable platform-api.service
```

### Method 3: Yocto recipe

1. Add `meta-hailo-camthink` and its `aipc-bootstrap` recipe to your layer.
2. Add `IMAGE_INSTALL += "aipc-bootstrap"` to your image recipe.
3. Build the image: `bitbake <your-image>`.

The recipe installs only the OS-owned launcher/units; the AIPC release tree is
applied to the persistent `/data` partition on first boot
(`aipc-restore`/`aipc-firstboot`) or via OTA.

## Service Configuration

### Service Dependencies

Boot ordering (see `systemd/aipc-platform.target` and `systemd/*.service`):

```text
aipc-restore (verify + restore network/SSH/AIPC from /data)
    |
aipc-firstboot (install AIPC release onto /data)
    |
network.target
    |
event-bus  ai-runtime  camera-daemon -> device-control
    \         |
     \        v
      +---> app-manager (Wants ai-runtime, event-bus)
              |
              v
         platform-api (After/Wants all services above)
```

All application services also `Wants=aipc-restore.service` and
`Requires=aipc-firstboot.service`. `camera-daemon` additionally depends on the
OS `isp_media_server.service` and `hailort_server.service`; `device-control`
runs after `aipc-mcu-prep.service`.

### Configuration File Locations

All configuration files are located at `/etc/aipc/` on the rootfs and at
`/data/aipc/etc/` on the device:
- `ai-runtime.yaml`
- `app-manager.yaml`
- `device-control.yaml`
- `event-bus.yaml`
- `platform-api.yaml`

### Runtime Directories

- `/data/aipc/logs/` — log files
- `/data/aipc/apps/registry/` — app metadata database
- `/data/aipc/apps/instances/` — running app instances
- `/data/aipc/etc/security/` — seccomp profile, event ACL
- `/run/aipc/` — Unix socket files

## Verify Deployment

### 1. Check Service Status

```bash
systemctl status ai-runtime.service
systemctl status app-manager.service
systemctl status device-control.service
systemctl status event-bus.service
systemctl status platform-api.service
```

### 2. Check Logs

```bash
journalctl -u ai-runtime.service -f
journalctl -u app-manager.service -f
```

### 3. Test API

```bash
# Platform API binds 127.0.0.1:8080; the health endpoint is public
curl http://localhost:8080/api/v1/system/health

# Or use CLI
/usr/bin/aipc-cli status
```

### 4. Check Processes

```bash
ps aux | grep -E "ai-runtime|app-manager|device-control|event-bus|platform-api"
```

## Common Issues

### Q: Service Fails to Start

**A:** Check:
1. Is containerd running: `systemctl status containerd`
2. Do configuration files exist: `ls -la /etc/aipc/*.yaml`
3. Logs: `journalctl -u <service-name> -n 50`
4. Did `aipc-restore`/`aipc-firstboot` complete? `systemctl status aipc-firstboot`

### Q: Cross-Compilation of C/C++ Fails

**A:**
1. Install the Yocto Poky SDK (Hailo developer portal) at `/opt/hailo-sdk` or
   set `SDK_PATH`.
2. Do **not** use the Ubuntu distro `gcc-aarch64-linux-gnu` — it lacks the Hailo
   sysroot and HAL libraries.
3. Source the SDK environment and verify `echo $CC` shows
   `aarch64-poky-linux-gcc`.

### Q: Go Service Cannot Find Dependencies

**A:**
1. Ensure `go.mod` exists and dependencies are downloaded
2. Check `GOPROXY` and network connection
3. Use `go mod download` to manually download

### Q: systemd Service Cannot Find Binary

**A:**
1. Check binary path: service files use `/usr/bin/<service-name>`
2. Ensure the install script correctly copied files
3. Verify permissions: `ls -la /usr/bin/ai-runtime`
4. In the on-device model, `/usr/bin` symlinks are rebuilt by `aipc-restore`
   from `/data/aipc/bin` — check that `/data/aipc/bin` exists and is populated.

## Next Steps

- Configure hardware-specific settings (HAL, device nodes, etc.)
- Adjust resource limits (MemoryLimit, CPUQuota in systemd service files)
- Configure log rotation
- Set up monitoring and alerting
