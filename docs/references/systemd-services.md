# Systemd Service Configuration

## Overview

The AIPC platform manages service lifecycles on Hailo-15 devices through systemd.
Two groups of units exist:

- **Application services** — the runtime binaries (AI, camera, event bus, …).
  Installed as `/usr/bin/<name>` executables (symlinks rebuilt by `aipc-restore`
  from `/data/aipc/bin`).
- **OS-level units** — boot/restore/upgrade plumbing shipped by the
  `aipc-bootstrap` package. Their executables live in `/usr/libexec` or run from
  `/data/aipc`.

## Application Services

| Service | Unit File | Description |
|---------|-----------|-------------|
| event-bus | `event-bus.service` | Message bus (UDP/TCP + Unix socket) |
| ai-runtime | `ai-runtime.service` | AI inference |
| camera-daemon | `camera-daemon.service` | Media pipeline and RTSP |
| device-control | `device-control.service` | Device/MCU control |
| app-manager | `app-manager.service` | Application/container management |
| device-discovery | `device-discovery.service` | CT-Disc device discovery |
| platform-api | `platform-api.service` | Web API gateway |

All application units:

- `Wants=aipc-restore.service` and `Requires=aipc-firstboot.service`
- `After=aipc-restore.service aipc-firstboot.service network.target`
- `ExecStart=/usr/bin/<name> -config /data/aipc/etc/<name>.yaml`
  (`camera-daemon` uses `-c`, `device-discovery` uses `--config`)
- `WantedBy=multi-user.target`

Additional dependencies:

| Unit | Depends on |
|------|-----------|
| `ai-runtime.service` | `Wants=containerd.service` |
| `camera-daemon.service` | `After=isp_media_server.service hailort_server.service aipc-mcu-prep.service`, `Before=ai-runtime.service` |
| `event-bus.service` | `Before=ai-runtime.service app-manager.service platform-api.service` |
| `device-control.service` | `After=camera-daemon.service aipc-mcu-prep.service`, `Wants=network-online.target` |
| `app-manager.service` | `Wants=containerd.service ai-runtime.service event-bus.service` |
| `platform-api.service` | `After` and `Wants` every other application service |

## OS-Level Units

| Unit | Type | Description |
|------|------|-------------|
| `aipc-restore.service` | oneshot | Verify `SHA256SUMS` and restore network/SSH/AIPC from the OS-upgrade backup; rebuilds `/usr/bin` and `/usr/libexec` symlinks. Runs **before** `network-pre.target` |
| `aipc-firstboot.service` | oneshot | Install/refresh the AIPC release tree on `/data/aipc` (execs `/usr/libexec/aipc-firstboot`) |
| `aipc-autostart.service` | oneshot | Enable and start AIPC services after OS restore |
| `aipc-healthmon.service` | oneshot | Black-box health sampler (`/data/aipc/scripts/aipc-healthmon.sh`) |
| `aipc-logrotate.service` | oneshot | AIPC Go-service log rotation (no built-in rotation in `logger.go`) |
| `aipc-logrotate.timer` | timer | Runs rotation 5 min after boot, then every 10 min (`AccuracySec=30s`, `Persistent=true`, idle I/O class) |
| `aipc-mcu-prep.service` | oneshot | Baseboard MCU boot prep: RTC sync + firmware OTA (`/data/aipc/bin/aipc-mcu-prep.sh`) |
| `aipc-nginx-gateway.service` | simple | Nginx app gateway + route sync (`aipc-nginx-app-route-sync.py --serve`); after `platform-api` and `app-manager` |
| `aipc-os-updater.service` | oneshot | A/B OS upgrade installer (`/usr/libexec/aipc-os-updater install`); writes only the inactive copy |
| `aipc-os-reboot.service` | oneshot | Reboot into the newly installed OS copy (`/usr/libexec/aipc-os-updater reboot`) |
| `aipc-os-verify.service` | oneshot | Post-upgrade verification (`/usr/libexec/aipc-os-updater verify`); rolls back and reboots on failure |
| `aipc-platform.target` | target | Stable grouping handle for the application platform. `Wants=` healthmon, event-bus, camera-daemon, ai-runtime, device-control, device-discovery, platform-api, app-manager, nginx-gateway |

## Startup Order

```
local-fs.target
   |
aipc-restore.service            # before network-pre.target
   |
aipc-firstboot.service          # after restore, before containerd
   |
network.target
   |
aipc-mcu-prep.service ──► camera-daemon.service ──► ai-runtime.service
   │                                 (after isp_media_server,
   │                                  hailort_server, mcu-prep)
   └──► device-control.service ──► event-bus.service
                                  (before ai-runtime/app-manager/platform-api)
   |
app-manager.service (Wants ai-runtime, event-bus, containerd)
   |
platform-api.service (After/Wants all app services)
   |
aipc-autostart.service
   |
aipc-platform.target
   |
aipc-os-verify.service          # post-upgrade verification
```

- `aipc-restore` runs before `network-pre.target` so network/SSH come up with the
  restored configuration.
- `aipc-os-updater` writes only the inactive A/B copy; `aipc-os-verify` checks
  the selected copy and rolls back on failure.

## Unit File Template

Application services follow this pattern (names and paths reflect the on-device
release layout, not the legacy `/opt/aipc` root):

```ini
[Unit]
Description=AIPC <Service Name>
Wants=aipc-restore.service
Requires=aipc-firstboot.service
After=aipc-restore.service aipc-firstboot.service network.target

[Service]
Type=simple
ExecStart=/usr/bin/<binary> -config /data/aipc/etc/<name>.yaml
Restart=on-failure
RestartSec=5
StartLimitBurst=5
StartLimitIntervalSec=60

StandardOutput=journal
StandardError=journal
SyslogIdentifier=aipc-<service>

# Resource limits
LimitNOFILE=65536
LimitNPROC=4096

[Install]
WantedBy=multi-user.target
```

## Common Commands

```bash
# View service status
systemctl status platform-api

# Start/stop/restart
sudo systemctl start camera-daemon
sudo systemctl stop ai-runtime
sudo systemctl restart event-bus

# Enable/disable auto-start on boot
sudo systemctl enable platform-api
sudo systemctl disable device-discovery

# View logs
journalctl -u camera-daemon -f
journalctl -u ai-runtime --since "1 hour ago"

# View all AIPC service statuses
systemctl status 'aipc-*'
```

## CLI Management

```bash
# Via aipc-cli
aipc-cli system start        # Start all services in dependency order
aipc-cli system stop         # Stop in reverse order
aipc-cli system restart      # Restart
aipc-cli system status       # View status
aipc-cli system health       # Health check
aipc-cli system enable       # Enable auto-start on boot
aipc-cli system disable      # Disable auto-start
```

## Service File Locations

- Unit files (OS-owned, always from the current OS image):
  `/etc/systemd/system/aipc-*.service` and `/etc/systemd/system/*.service`
- Executables: `/usr/bin/<name>` (symlinks to `/data/aipc/bin`, rebuilt by
  `aipc-restore`); OS plumbing in `/usr/libexec`
- Configuration files: `/data/aipc/etc/` (canonical) / `/etc/aipc/` (rootfs pre-seed)
- Log files: `/data/aipc/logs/`
- Runtime sockets: `/run/aipc/`

## Log Management

Service logs are written to both journald and files:

- **journald**: `journalctl -u <service>`
- **File**: `/data/aipc/logs/<service>.log` (specified by `log_file` in configuration)
- Application container logs: `/data/aipc/logs/apps/`
- Rotation: `aipc-logrotate.timer` (5 min after boot, then every 10 min)
