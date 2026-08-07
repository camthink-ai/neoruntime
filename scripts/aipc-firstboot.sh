#!/bin/bash
# ============================================================================
# AIPC Platform - Hailo Device Boot Initialization
# ============================================================================
# This script runs on every boot of a Hailo-15 SoC device to prepare the
# runtime environment for NE503 AIPC platform services.
#
# Static setup (directories, symlinks, configs, udev rules, ldconfig,
# tmpfiles.d, seccomp profile) is handled by the packaging/installation
# process (pack-release). This script only handles dynamic, per-boot
# initialization that cannot be done at install time.
#
# Usage:
#   ./aipc-firstboot.sh          # Normal boot (idempotent)
#   ./aipc-firstboot.sh --status  # Show current status only
# ============================================================================

set -euo pipefail
shopt -s nullglob

# ---------- flock: prevent parallel execution ----------
exec 200>/var/lock/aipc-firstboot.lock
flock -n 200 || { echo "[aipc-boot] Already running, exiting"; exit 0; }

# ---------- Colors ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${GREEN}[aipc-boot]${NC} $(date '+%H:%M:%S') $*"; }
warn() { echo -e "${YELLOW}[aipc-boot]${NC} $(date '+%H:%M:%S') WARN: $*"; }
err()  { echo -e "${RED}[aipc-boot]${NC} $(date '+%H:%M:%S') ERROR: $*" >&2; }
info() { echo -e "${CYAN}[aipc-boot]${NC} $(date '+%H:%M:%S') $*"; }

# ---------- Root check ----------
if [[ $EUID -ne 0 ]]; then
    err "Must run as root"
    exit 1
fi

# ---------- Parse args ----------
ACTION="boot"
while [[ $# -gt 0 ]]; do
    case "$1" in
        --status)  ACTION="status";  shift ;;
        -h|--help)
            echo "Usage: $0 [--status]"
            echo "  (default)  Normal boot initialization (idempotent)"
            echo "  --status   Show platform status only"
            exit 0
            ;;
        *) err "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---------- Boot banner (serial / console) ----------
# Printed once at the very top of boot so the serial debug console shows a
# clear marker: version + platform + wall clock. Skipped in --status mode.
# /data/aipc/VERSION is a key=value file (version=, platform=, build_date=,
# git_commit=) written by pack-release; source defensively so a missing or
# malformed file never aborts boot.
if [[ "$ACTION" == "boot" ]]; then
    _boot_ver="unknown"
    _boot_platform="$(uname -m 2>/dev/null || echo unknown)"
    if [[ -f /data/aipc/VERSION ]]; then
        . /data/aipc/VERSION 2>/dev/null || true
        [[ -n "${version:-}" ]]  && _boot_ver="$version"
        [[ -n "${platform:-}" ]] && _boot_platform="$platform"
    fi
    printf '\n%b\n%b\n%b\n\n' \
        "${CYAN}============================================================${NC}" \
        "${CYAN}   AIPC BOOT   ${_boot_ver}   ${_boot_platform}   $(date '+%Y-%m-%d %H:%M:%S')${NC}" \
        "${CYAN}============================================================${NC}"
fi

# ---------- Constants ----------
# Minimum plausible epoch — reject timestamps before 2024-01-01 (bad RTC / empty battery)
MIN_PLAUSIBLE_TS=1704067200

# Detect install prefix for status display. /data/aipc is the sole canonical
# root; legacy /data (flat) and /opt/aipc (rootfs) are kept as transition
# fallbacks for pre-migration devices.
detect_prefix() {
    if [[ -d /data/aipc/bin ]] && [[ -f /data/aipc/bin/platform-api || -f /data/aipc/etc/platform-api.yaml ]]; then
        echo "/data/aipc"
    elif [[ -d /data/bin ]] && [[ -f /data/bin/platform-api || -f /data/etc/platform-api.yaml ]]; then
        echo "/data"
    elif [[ -d /opt/aipc/bin ]]; then
        echo "/opt/aipc"
    else
        echo "/data/aipc"
    fi
}
AIPC_PREFIX="${AIPC_PREFIX:-$(detect_prefix)}"
AIPC_ETC="${AIPC_PREFIX}/etc"

# All NE503 platform services (for status display only)
SERVICES_ORDERED=(
    event-bus
    camera-daemon
    ai-runtime
    device-control
    device-discovery
    platform-api
    app-manager
)

fix_systemd_log_level_env() {
    local current_env unit patched=0

    if [[ "${SYSTEMD_LOG_LEVEL:-}" == "error" ]]; then
        export SYSTEMD_LOG_LEVEL=err
        log "Systemd process env SYSTEMD_LOG_LEVEL: error -> err"
    fi

    current_env="$(systemctl show-environment 2>/dev/null | sed -n 's/^SYSTEMD_LOG_LEVEL=//p' | head -1 || true)"
    if [[ "$current_env" == "error" ]]; then
        systemctl set-environment SYSTEMD_LOG_LEVEL=err 2>/dev/null && \
            log "Systemd manager env SYSTEMD_LOG_LEVEL: error -> err" || \
            warn "Failed to fix SYSTEMD_LOG_LEVEL manager env"
    fi

    for unit in \
        /etc/systemd/system/serial-log-level.service \
        /lib/systemd/system/serial-log-level.service \
        /usr/lib/systemd/system/serial-log-level.service; do
        [[ -f "$unit" ]] || continue
        if grep -q 'SYSTEMD_LOG_LEVEL=error' "$unit" 2>/dev/null; then
            if sed -i 's/SYSTEMD_LOG_LEVEL=error/SYSTEMD_LOG_LEVEL=err/g' "$unit" 2>/dev/null; then
                patched=1
                log "Patched invalid SYSTEMD_LOG_LEVEL in $unit"
            else
                warn "Failed to patch invalid SYSTEMD_LOG_LEVEL in $unit"
            fi
        fi
    done

    [[ $patched -eq 0 ]] || systemctl daemon-reload 2>/dev/null || true
}

fix_systemd_log_level_env

# ============================================================================
# Status check mode
# ============================================================================
if [[ "$ACTION" == "status" ]]; then
    echo ""
    echo "============================================"
    echo "  NE503 AIPC Platform Status"
    echo "============================================"
    echo ""

    # Version
    if [[ -f "${AIPC_ETC}/version" ]]; then
        echo "Version:        $(cat "${AIPC_ETC}/version" | tr -d '[:space:]')"
    fi
    echo "Install prefix: ${AIPC_PREFIX}"

    # Kernel driver / NPU
    if [[ -e /dev/h1x ]]; then
        echo -e "  NPU:            ${GREEN}/dev/h1x (integrated)${NC}"
    elif [[ -e /dev/hailo0 ]]; then
        echo -e "  NPU:            ${GREEN}/dev/hailo0 (PCIe)${NC}"
    else
        echo -e "  NPU:            ${RED}NOT found${NC}"
    fi

    # RTC
    if [[ -e /dev/rtc0 ]]; then
        rtc_time=$(hwclock -r 2>/dev/null || echo "n/a")
        echo -e "  RTC:            ${GREEN}present${NC} (${rtc_time})"
    else
        echo -e "  RTC:            ${YELLOW}not found${NC}"
    fi

    # Device nodes
    for dev in /dev/hailo0 /dev/video0; do
        if [[ -e "$dev" ]]; then
            echo -e "  ${dev}:  ${GREEN}present${NC}"
        else
            echo -e "  ${dev}:  ${RED}missing${NC}"
        fi
    done

    # Services
    for svc in "${SERVICES_ORDERED[@]}"; do
        status=$(systemctl is-active "$svc" 2>/dev/null || echo "unknown")
        enabled=$(systemctl is-enabled "$svc" 2>/dev/null || echo "unknown")
        case "$status" in
            active)   echo -e "  ${svc}:  ${GREEN}${status}${NC} (${enabled})" ;;
            inactive|failed) echo -e "  ${svc}:  ${RED}${status}${NC} (${enabled})" ;;
            *)        echo -e "  ${svc}:  ${YELLOW}${status}${NC} (${enabled})" ;;
        esac
    done

    # API health
    if curl -sf http://localhost:8080/api/v1/system/health >/dev/null 2>&1; then
        echo -e "  Platform API:   ${GREEN}responding${NC}"
    else
        echo -e "  Platform API:   ${YELLOW}not responding${NC}"
    fi
    echo ""
    exit 0
fi

# ============================================================================
# Step 0: Per-device network provisioning (survives rootfs upgrade)
# ============================================================================
# The OS image ships a generic /etc/systemd/network/10-eth0.network
# (Address=10.0.0.1/24 fallback). A commissioned device's real static IP
# cannot be baked into the image, so it is persisted on /data (which the
# single-recovery upgrade never rewrites) as *.network files and promoted
# to /etc here on every boot.
#
# Without this, every rootfs upgrade reverts eth0 to the image default — or
# worse, to an empty file restored from a network.tar.gz backup that was
# captured while the commissioned IP lived only in runtime (set via
# `ip addr add`, never written to disk). The device then comes up with no
# reachable IP and looks like a brick. Persisting on /data + promoting here
# is the image-level root cure.
#
# Commissioning contract: write the per-device config to
#   /data/aipc/network/10-eth0.network   (and 20-eth1.network, etc.)
# using standard systemd .network syntax. firstboot does the rest.
provision_network() {
    local src_dir="/data/aipc/network"
    local dst_dir="/etc/systemd/network"
    local changed=0

    # Uncommissioned device — keep the image default.
    [[ -d "$src_dir" ]] || return 0

    shopt -s nullglob
    local src base dst iface
    for src in "$src_dir"/*.network; do
        base="$(basename "$src")"
        dst="$dst_dir/$base"
        if [[ ! -f "$dst" ]] || ! cmp -s "$src" "$dst"; then
            install -D -m 0644 "$src" "$dst"
            changed=1
            log "provisioned $base from $src_dir"
        fi
    done

    if [[ $changed -eq 1 ]] && command -v networkctl >/dev/null 2>&1; then
        # Reload so networkd picks up the new .network files, then reconfigure
        # each matched interface so the new IP takes effect even if networkd
        # had already brought it up with the image default at early boot.
        networkctl reload 2>/dev/null || true
        for src in "$src_dir"/*.network; do
            iface="$(awk -F= '/^[[:space:]]*Name[[:space:]]*=/{gsub(/[[:space:]]/,"",$2); print $2; exit}' "$src" 2>/dev/null)"
            if [[ -n "$iface" ]]; then
                networkctl reconfigure "$iface" 2>/dev/null || true
            fi
        done
    fi
}
provision_network

# ============================================================================
# Step 1: Kernel modules (Hailo NPU + RTC)
# ============================================================================
log "=== [1/5] Loading kernel modules ==="

# --- Hailo NPU ---
# Hailo-15 SoC uses integrated NPU (hailo_integrated_nnc → /dev/h1x).
# Hailo-8 PCIe uses discrete NPU (hailo_pci → /dev/hailo0).
NPU_DEVICE=""
if [[ -e /dev/h1x ]]; then
    NPU_DEVICE="/dev/h1x"
    info "Hailo integrated NPU: /dev/h1x (already present)"
elif [[ -e /dev/hailo0 ]]; then
    NPU_DEVICE="/dev/hailo0"
    info "Hailo PCIe NPU: /dev/hailo0 (already present)"
else
    # Try loading integrated NPU first (Hailo-15 SoC), then PCIe (Hailo-8)
    modprobe hailo_integrated_nnc 2>/dev/null && log "hailo_integrated_nnc loaded" || true
    modprobe hailo_pci 2>/dev/null && log "hailo_pci loaded" || true

    # Wait for either device node (up to 5s)
    for i in $(seq 1 10); do
        if [[ -e /dev/h1x ]]; then
            NPU_DEVICE="/dev/h1x"
            break
        fi
        if [[ -e /dev/hailo0 ]]; then
            NPU_DEVICE="/dev/hailo0"
            break
        fi
        sleep 0.5
    done
fi

if [[ -n "${NPU_DEVICE}" ]]; then
    log "NPU device: ${NPU_DEVICE}"
else
    warn "No NPU device found (/dev/h1x or /dev/hailo0) — AI inference unavailable"
fi

# --- RTC (HYM8563 via I2C) ---
RTC_MODULE=""
for mod in rtc_hym8563 rtc-hym8563 rtc_pcf85363 rtc-pcf85363 rtc_rv3028 rtc-rv3028; do
    if modprobe "${mod}" 2>/dev/null; then
        RTC_MODULE="${mod}"
        log "RTC module loaded: ${mod}"
        break
    fi
done

if [[ -z "${RTC_MODULE}" ]] && ls /sys/class/rtc/rtc0 >/dev/null 2>&1; then
    info "RTC already registered (built-in)"
fi

rtc_ready=false
for i in $(seq 1 6); do
    if [[ -e /dev/rtc0 ]]; then
        rtc_ready=true
        break
    fi
    sleep 0.5
done

if $rtc_ready; then
    log "RTC device node: /dev/rtc0"
else
    warn "No hardware RTC found — system time relies on NTP only"
fi

# ============================================================================
# Step 2: Time synchronization
# ============================================================================
# Restore system time from RTC or last-known-time, then ensure NTP is running.
# Application-level config (NTP server, timezone) is managed by platform-api
# TimeHandler via /api/v1/system/time/config — do NOT set those here.
log "=== [2/5] Time synchronization ==="

# Sync hardware RTC → system clock if RTC is present and plausible
if [[ -e /dev/rtc0 ]]; then
    rtc_ts=$(hwclock -u --show 2>/dev/null | xargs -I{} date -d "{}" +%s 2>/dev/null || echo "0")
    if [[ "${rtc_ts}" -ge "${MIN_PLAUSIBLE_TS}" ]]; then
        hwclock --hctosys 2>/dev/null && log "System clock synced from hardware RTC" || \
            warn "hwclock --hctosys failed"
    else
        warn "RTC time is implausible (ts=${rtc_ts}), skipping hctosys"
    fi
fi

# Restore time from last-known-time if system clock is stale
# (covers devices without RTC and without network at boot)
current_ts=$(date +%s)
LAST_KNOWN_TIME="${AIPC_ETC}/last-known-time.json"

if [[ "${current_ts}" -lt "${MIN_PLAUSIBLE_TS}" ]] && [[ -f "${LAST_KNOWN_TIME}" ]]; then
    saved_ts=$(grep -o '"unix_timestamp": *[0-9]*' "${LAST_KNOWN_TIME}" 2>/dev/null | grep -o '[0-9]*' || true)
    if [[ -n "${saved_ts}" ]] && [[ "${saved_ts}" -gt "${MIN_PLAUSIBLE_TS}" ]]; then
        saved_date=$(date -d "@${saved_ts}" '+%Y-%m-%d %H:%M:%S' 2>/dev/null)
        if [[ -n "${saved_date}" ]]; then
            systemctl stop systemd-timesyncd.service ntpd.service chronyd.service 2>/dev/null || true
            timedatectl set-time "${saved_date}" 2>/dev/null && \
                log "System time restored from last-known-time: ${saved_date}" || \
                warn "Failed to restore time from last-known-time"
        fi
    else
        warn "last-known-time is empty or implausible, cannot restore"
    fi
elif [[ "${current_ts}" -lt "${MIN_PLAUSIBLE_TS}" ]]; then
    warn "System clock is stale (ts=${current_ts}) and no last-known-time available"
else
    info "System time is current ($(date '+%Y-%m-%d %H:%M:%S'))"
fi

# Guard: dbus-org.freedesktop.timesync1.service must be a symlink to
# systemd-timesyncd.service (its [Install] Alias). A stray standalone copy
# here makes systemd refuse timesyncd with "Two services allocated for the
# same bus name" → systemctl restart exits 1 → platform-api time save 500s.
_TIMESYNC_ALIAS="/etc/systemd/system/dbus-org.freedesktop.timesync1.service"
if [[ -e "${_TIMESYNC_ALIAS}" && ! -L "${_TIMESYNC_ALIAS}" ]]; then
    rm -f "${_TIMESYNC_ALIAS}"
    systemctl daemon-reload 2>/dev/null || true
    warn "Removed stray duplicate systemd-timesyncd unit (${_TIMESYNC_ALIAS})"
fi

# Select timesyncd ahead of other providers. timedated otherwise prefers the
# vendor 60-ntpd.list and disables timesyncd every time set-ntp=true is called.
mkdir -p /etc/systemd/ntp-units.d
printf '%s\n' 'systemd-timesyncd.service' > \
    /etc/systemd/ntp-units.d/10-aipc-timesyncd.list

for conflicting_ntp in ntpd.service chronyd.service; do
    if systemctl show --property LoadState --value "$conflicting_ntp" 2>/dev/null | \
       grep -qv '^not-found$'; then
        systemctl disable --now "$conflicting_ntp" || \
            warn "Failed to disable conflicting NTP provider $conflicting_ntp"
    fi
done

# Ensure the provider is enabled persistently as well as active now. Merely
# observing an active D-Bus-activated instance does not guarantee it will start
# after the next power cycle.
if systemctl enable --now systemd-timesyncd.service; then
    if systemctl is-enabled --quiet systemd-timesyncd.service && \
       systemctl is-active --quiet systemd-timesyncd.service; then
        log "NTP time sync enabled and active"
    else
        warn "systemd-timesyncd enable/start verification failed"
    fi
else
    warn "Failed to enable and start systemd-timesyncd"
fi

info "System time: $(date '+%Y-%m-%d %H:%M:%S %Z')"

# ============================================================================
# Step 3: Device permissions
# ============================================================================
# udev rules (installed by pack-release) handle permissions on device
# appearance. This step applies permissions to devices that are already
# present at boot time, before udev finishes processing rules.
log "=== [3/5] Setting device permissions ==="

# Ensure aipc group exists for device access
if ! getent group aipc >/dev/null 2>&1; then
    groupadd -r aipc 2>/dev/null || true
fi

# Apply permissions to already-present devices
# (udev rules handle future device events)
for npu_dev in /dev/h1x /dev/hailo0; do
    if [[ -e "${npu_dev}" ]]; then
        chown root:aipc "${npu_dev}" 2>/dev/null || true
        chmod 660 "${npu_dev}" 2>/dev/null || true
    fi
done

if [[ -e /dev/video0 ]]; then
    chown root:aipc /dev/video0 2>/dev/null || true
    chmod 660 /dev/video0 2>/dev/null || true
fi

if [[ -d /dev/dma_heap ]]; then
    chown -R root:aipc /dev/dma_heap 2>/dev/null || true
    chmod -R 770 /dev/dma_heap 2>/dev/null || true
fi

if [[ -e /dev/ttyS0 ]]; then
    chown root:aipc /dev/ttyS0 2>/dev/null || true
    chmod 660 /dev/ttyS0 2>/dev/null || true
fi

log "Device permissions set"

# ============================================================================
# Step 4: Clean stale runtime files
# ============================================================================
# /run is tmpfs and wiped on reboot, but stale SHM segments from an
# unclean shutdown can persist. Also clean any leftover files from
# services that didn't shut down gracefully.
log "=== [4/5] Clean stale runtime files ==="

rm -f /dev/shm/aipc_* 2>/dev/null || true

log "Stale files cleaned"

# ============================================================================
# Step 5: Systemd environment — log level + serial console
# ============================================================================
# Fix two common embedded BSP misconfigurations that cause serial console hangs:
# 1. systemd log-level=debug floods the journal with D-Bus traces, which
#    triggers journal rotation storms that spam the kernel console (serial).
# 2. serial-getty may be disabled or targeting the wrong UART device.
# These fixes are idempotent and safe to run on every boot.
log "=== [5/5] Systemd environment setup ==="

# Ensure log level is info (not debug) — prevents D-Bus journal flood
current_loglevel=$(systemctl log-level 2>/dev/null || echo "unknown")
if [[ "$current_loglevel" != "info" ]]; then
    systemctl log-level info 2>/dev/null && log "Systemd log level: ${current_loglevel} → info" || \
        warn "Failed to set systemd log level"
else
    info "Systemd log level already info"
fi

# Ensure serial-getty is running on the kernel console device
# (console=ttySX on kernel cmdline). systemd-getty-generator usually handles
# this, but some BSPs ship with it masked or targeting the wrong device.
if ! systemctl is-active --quiet serial-getty@* 2>/dev/null; then
    console_dev=$(grep -o 'console=tty[^ ,]*' /proc/cmdline 2>/dev/null | head -1 | sed 's/console=//' | cut -d, -f1)
    if [[ -n "${console_dev}" ]] && [[ -e "/dev/${console_dev}" ]]; then
        info "Enabling serial-getty on /dev/${console_dev}"
        systemctl enable --now "serial-getty@${console_dev}.service" 2>/dev/null && \
            log "serial-getty@${console_dev} enabled" || \
            warn "Failed to enable serial-getty@${console_dev}"
    fi
else
    info "Serial getty already running"
fi

# ============================================================================
# Step 6: Health-monitoring infrastructure (persistent logs + self-heal)
# ============================================================================
# Prepare on-disk artifacts that make a FULL HANG (the 93.72 failure mode:
# SSH/network dead, device unreachable) diagnosable after the reboot.
# Idempotent and safe to run on every boot.
#   /data/health  - healthmon black-box snapshots (aipc-healthmon.sh)
#   /data/core    - persistent coredumps (kernel.core_pattern points here)
#   /data/journal - bind-mounted onto /var/log/journal so journald persists
#   /data/aipc-data - databases, models, app state and compatibility schema
log "=== Step 6: Health-monitoring infrastructure ==="

# 6a. Persistent data directories
for d in /data/health /data/core /data/journal /data/logs \
         /data/aipc-data/database /data/aipc-data/models \
         /data/aipc-data/apps /data/aipc-data/containerd; do
    mkdir -p "$d" 2>/dev/null || true
done

containerd_config_root() {
    [[ -f /etc/containerd/config.toml ]] || return 0
    sed -n 's/^[[:space:]]*root[[:space:]]*=[[:space:]]*"\([^"]*\)".*/\1/p' \
        /etc/containerd/config.toml 2>/dev/null | head -1 || true
}

containerd_set_config_value() {
    local key="$1" value="$2" config="/etc/containerd/config.toml" tmp

    if grep -q "^[[:space:]]*${key}[[:space:]]*=" "$config" 2>/dev/null; then
        sed -i -E "s|^[[:space:]]*${key}[[:space:]]*=.*|${key} = \"${value}\"|" "$config"
        return 0
    fi

    tmp="$(mktemp)"
    {
        printf '%s = "%s"\n' "$key" "$value"
        cat "$config"
    } >"$tmp"
    cat "$tmp" >"$config"
    rm -f "$tmp"
}

containerd_set_config_version() {
    local config="/etc/containerd/config.toml" tmp

    if grep -q '^[[:space:]]*version[[:space:]]*=' "$config" 2>/dev/null; then
        sed -i -E 's|^[[:space:]]*version[[:space:]]*=.*|version = 2|' "$config"
        return 0
    fi

    tmp="$(mktemp)"
    {
        printf 'version = 2\n'
        cat "$config"
    } >"$tmp"
    cat "$tmp" >"$config"
    rm -f "$tmp"
}

migrate_containerd_root() {
    local source="$1" destination="$2" link_target

    [[ -n "$source" && "$source" != "$destination" ]] || return 0
    mkdir -p "$destination" 2>/dev/null || {
        warn "Failed to create containerd root $destination"
        return 0
    }

    if [[ -L "$source" ]]; then
        link_target="$(readlink -f "$source" 2>/dev/null || true)"
        if [[ "$link_target" == "$destination" ]]; then
            return 0
        fi
        if [[ -n "$link_target" && -d "$link_target" ]]; then
            cp -a "$link_target"/. "$destination"/ 2>/dev/null || true
        fi
        rm -f "$source" 2>/dev/null || true
    elif [[ -d "$source" ]]; then
        cp -a "$source"/. "$destination"/ 2>/dev/null || true
        rm -rf "$source" 2>/dev/null || warn "Failed to remove old containerd root $source"
    elif [[ -e "$source" ]]; then
        warn "Cannot migrate containerd root because it is not a directory: $source"
        return 0
    fi

    mkdir -p "$(dirname "$source")" 2>/dev/null || true
    ln -s "$destination" "$source" 2>/dev/null || true
}

configure_containerd_root() {
    local target="/data/aipc-data/containerd"
    local config="/etc/containerd/config.toml"
    local current_root varlib_target
    local needs_fix=0
    local restart_containerd=0
    local restart_app_manager=0
    local restart_ai_runtime=0

    mkdir -p "$target" 2>/dev/null || {
        warn "Failed to prepare containerd root $target"
        return 0
    }

    current_root="$(containerd_config_root)"
    [[ -n "$current_root" ]] || current_root="/var/lib/containerd"
    varlib_target="$(readlink -f /var/lib/containerd 2>/dev/null || true)"

    [[ -f "$config" && "$current_root" == "$target" ]] || needs_fix=1
    [[ -e /var/lib/containerd || -L /var/lib/containerd ]] || needs_fix=1
    [[ "$varlib_target" == "$target" ]] || needs_fix=1

    if [[ $needs_fix -eq 0 ]]; then
        log "containerd root ready: $target"
        return 0
    fi

    # Normal boot ordering keeps containerd behind firstboot. If this runs
    # manually on an already booted device, stop and restore affected services.
    if systemctl is-active --quiet containerd 2>/dev/null; then
        restart_containerd=1
        systemctl is-active --quiet app-manager 2>/dev/null && restart_app_manager=1
        systemctl is-active --quiet ai-runtime 2>/dev/null && restart_ai_runtime=1
        systemctl stop app-manager ai-runtime containerd 2>/dev/null || true
    fi

    migrate_containerd_root "$current_root" "$target"
    migrate_containerd_root "/var/lib/containerd" "$target"

    mkdir -p /etc/containerd 2>/dev/null || true
    if [[ ! -f "$config" ]]; then
        cat >"$config" <<EOF
root = "$target"
state = "/run/containerd"
version = 2
EOF
    else
        containerd_set_config_value root "$target"
        containerd_set_config_value state "/run/containerd"
        containerd_set_config_version
    fi

    log "containerd root ready: $target"

    if [[ $restart_containerd -eq 1 ]]; then
        systemctl start --no-block containerd 2>/dev/null || warn "Failed to restart containerd after root migration"
        [[ $restart_ai_runtime -eq 1 ]] && systemctl start --no-block ai-runtime 2>/dev/null || true
        [[ $restart_app_manager -eq 1 ]] && systemctl start --no-block app-manager 2>/dev/null || true
    fi
}

configure_containerd_root

chmod 1777 /data/core 2>/dev/null || true   # any crashing process may dump core here
# Bound /data/core: keep only the 8 most recent cores so a crash loop (e.g. a
# C++ daemon with large buffers) can't fill the partition. Runs every boot.
if [[ -d /data/core ]]; then
    ls -t /data/core/core.* 2>/dev/null | tail -n +9 | while read -r old; do
        rm -f "$old" 2>/dev/null
    done || true
fi
if [[ "$AIPC_PREFIX" == "/opt/aipc" || "$AIPC_PREFIX" == "/data/aipc" ]]; then
    for mapping in \
        "${AIPC_PREFIX}/data:/data/aipc-data/database" \
        "${AIPC_PREFIX}/models:/data/aipc-data/models" \
        "${AIPC_PREFIX}/apps:/data/aipc-data/apps"; do
        source_path="${mapping%%:*}"
        destination_path="${mapping#*:}"
        if [[ -L "$source_path" ]] &&
           [[ "$(readlink -f "$source_path")" == "$(readlink -f "$destination_path")" ]]; then
            continue
        fi
        if [[ -d "$source_path" && ! -L "$source_path" ]]; then
            cp -a "$source_path"/. "$destination_path"/ 2>/dev/null || true
            rm -rf "$source_path"
        elif [[ -L "$source_path" ]]; then
            old_target="$(readlink -f "$source_path" 2>/dev/null || true)"
            if [[ -n "$old_target" && -d "$old_target" ]]; then
                cp -a "$old_target"/. "$destination_path"/ 2>/dev/null || true
            fi
            rm -f "$source_path"
        fi
        ln -s "$destination_path" "$source_path" 2>/dev/null ||
            warn "Failed to link $source_path -> $destination_path"
    done
fi
log "Persistent dirs ready: /data/{health,core,journal,logs,aipc-data}"

# 6c. Install fleet IMU calibration (gyro bias + mount rotation) shipped in the
#     AIPC package to the app-resource path the platform-api gyro attitude
#     endpoint reads from. Idempotent + best-effort: missing source -> no-op,
#     so gyro falls back to the static mount_matrix in platform-api.yaml.
#     cp -f makes the firmware-shipped calibration authoritative; flip to
#     cp -n if a unit is ever re-calibrated in the field and must be preserved.
CALIB_SRC="${AIPC_PREFIX}/share/calibration/final_calibration.json"
CALIB_DST="/home/root/apps/resources/final_calibration.json"
if [[ -f "$CALIB_SRC" ]]; then
    mkdir -p "$(dirname "$CALIB_DST")" 2>/dev/null || true
    if [[ -s "$CALIB_DST" ]]; then
        info "IMU calibration already present: $CALIB_DST"
    elif cp -f "$CALIB_SRC" "$CALIB_DST" 2>/dev/null; then
        log "IMU calibration installed -> $CALIB_DST"
    else
        warn "IMU calibration copy failed ($CALIB_SRC -> $CALIB_DST)"
    fi
fi

# 6b. Apply kernel watchdog / panic self-heal sysctl (idempotent). Also
#     applied by systemd-sysctl.service at boot — this re-apply defends
#     against BSP overrides and is the single source of truth.
SYSCTL_CONF=/etc/sysctl.d/99-aipc-watchdog.conf
if [[ -f "$SYSCTL_CONF" ]]; then
    if sysctl -p "$SYSCTL_CONF" >/dev/null 2>&1; then
        log "Kernel self-heal sysctl applied"
    else
        warn "sysctl -p failed (some keys may be unsupported on this kernel)"
    fi
else
    warn "$SYSCTL_CONF missing — kernel self-heal NOT active"
fi

# 6b.1 Neutralize the BSP's /etc/sysctl.conf core_pattern override. The Hailo15
#      BSP ships an uncommented `kernel.core_pattern=/home/root/%e.%p.core` in
#      /etc/sysctl.conf. sysctl --system loads /etc/sysctl.conf LAST (after
#      /etc/sysctl.d/*.conf), so this BSP line silently overrides our
#      99-aipc-watchdog.conf on every boot and sends crash cores to the small
#      root partition (/home/root) instead of persistent /data/core — which
#      fills root and triggers the hang cascade. Comment it out so our drop-in
#      wins. Idempotent: the grep matches only an ACTIVE line, so re-runs are
#      no-ops. Backup once via cp -n.
if [[ -f /etc/sysctl.conf ]] && grep -qE '^[[:space:]]*kernel\.core_pattern[[:space:]]*=' /etc/sysctl.conf; then
    cp -n /etc/sysctl.conf /etc/sysctl.conf.bak.aipc 2>/dev/null || true
    sed -i -E 's|^([[:space:]]*kernel\.core_pattern[[:space:]]*=.*)|#\1  # neutralized by aipc-firstboot -> /etc/sysctl.d/99-aipc-watchdog.conf|' /etc/sysctl.conf
    log "Neutralized BSP /etc/sysctl.conf core_pattern override (-> /data/core)"
fi

# 6c. Bind-mount persistent /data/journal onto /var/log/journal so the journal
#     survives reboot. zz-aipc-journal-persist.conf forces Storage=persistent,
#     overriding the BSP's Storage=volatile. /var/log is tmpfs on this BSP;
#     without this bind the journal stays volatile.
JOURNAL_SRC=/data/journal
JOURNAL_DST=/var/log/journal
mkdir -p "$JOURNAL_DST" 2>/dev/null || true

# Keep exactly one active journal bind entry. The previous grep pattern did not
# match lines that started with /data/journal, so repeated boots could append
# duplicates and make systemd-fstab-generator fail during daemon-reexec.
if [[ -f /etc/fstab ]]; then
    tmp_fstab="$(mktemp)"
    awk -v src="$JOURNAL_SRC" -v dst="$JOURNAL_DST" '
        /^[[:space:]]*#/ || NF < 2 { print; next }
        $1 == src && $2 == dst {
            if (!seen) {
                print
                seen = 1
            }
            next
        }
        { print }
    ' /etc/fstab >"$tmp_fstab" && cat "$tmp_fstab" >/etc/fstab
    rm -f "$tmp_fstab"
fi
if ! grep -qE "^[[:space:]]*${JOURNAL_SRC}[[:space:]]+${JOURNAL_DST}[[:space:]]+" /etc/fstab 2>/dev/null; then
    echo "${JOURNAL_SRC} ${JOURNAL_DST} none bind 0 0" >> /etc/fstab || \
        warn "Failed to append journal bind mount to /etc/fstab"
fi
if ! findmnt --mountpoint "$JOURNAL_DST" >/dev/null 2>&1; then
    if mount "$JOURNAL_DST" 2>/dev/null || mount -o bind "$JOURNAL_SRC" "$JOURNAL_DST" 2>/dev/null; then
        log "Journal bind mount active: ${JOURNAL_SRC} -> ${JOURNAL_DST}"
    else
        warn "Journal bind mount failed — journal stays volatile this boot"
    fi
else
    info "Journal bind mount already active"
fi

# 6c.2 Adopt persistent storage. journald only switches to /var/log/journal if
#      /var/log/journal/<machine-id>/ already exists (it does not create the
#      dir itself), so create it on the bind mount, restart journald, then
#      flush the runtime journal onto the persistent location. Verified
#      required on systemd 250 / Hailo15 BSP — restart alone does NOT migrate;
#      the explicit flush does.
if findmnt --mountpoint "$JOURNAL_DST" >/dev/null 2>&1; then
    MID="$(cat /etc/machine-id 2>/dev/null)"
    if [[ -n "$MID" ]]; then
        # systemd-journal group may not exist on every BSP; fall back to root.
        install -d -o root -g systemd-journal -m 2755 "${JOURNAL_DST}/${MID}" 2>/dev/null || \
            install -d -m 2755 "${JOURNAL_DST}/${MID}" 2>/dev/null || true
    fi
    systemctl restart systemd-journald 2>/dev/null || true
    # Flush runtime -> persistent (systemd 245+). Older systemd: SIGUSR1.
    if ! journalctl --flush >/dev/null 2>&1; then
        systemctl kill --signal=SIGUSR1 systemd-journald 2>/dev/null || true
    fi
    log "Persistent journal adopted (runtime flushed -> /data/journal)"
else
    warn "Journal bind not active — persistent journal unavailable this boot"
fi

# 6d. Enable the healthmon black-box sampler. Service startup is deliberately
#     left to aipc-autostart.service, which runs only after this oneshot exits.
#     Starting healthmon synchronously here would deadlock because healthmon is
#     ordered After=aipc-firstboot.service.
if systemctl list-unit-files 2>/dev/null | grep -q '^aipc-healthmon.service'; then
    systemctl enable aipc-healthmon.service 2>/dev/null || true
    info "aipc-healthmon enabled; aipc-autostart will start it"
else
    warn "aipc-healthmon.service not installed — black-box sampling disabled"
fi

# 6e. Pin Docker's data-root to /data/docker. Docker defaults to /var/lib/docker
#     on the 3.3G root partition, where a few app images can fill root and
#     trigger the 93.72 full-hang failure mode. This writes daemon.json so every
#     boot (including post-reflash) keeps images/containers on the 54G /data
#     partition, and caps per-container json-file logs (max-size=10m, max-file=3)
#     so a chatty container can't grow logs without bound. Idempotent: rewrites
#     only when missing or differing from the desired config. Docker need not be
#     running. NOTE: this only sets the policy; a one-time migration of any
#     pre-existing /var/lib/docker content is a separate manual step (rsync +
#     rename), already performed on 93.72.
DOCKER_CONF=/etc/docker/daemon.json
DESIRED_DOCKER_CONF='{
  "data-root": "/data/docker",
  "storage-driver": "overlay2",
  "log-driver": "json-file",
  "log-opts": {
    "max-size": "10m",
    "max-file": "3"
  }
}'
if [[ -d /data ]]; then
    mkdir -p /data/docker /etc/docker 2>/dev/null || true
    if [[ ! -f "$DOCKER_CONF" ]] || ! printf '%s\n' "$DESIRED_DOCKER_CONF" | diff - "$DOCKER_CONF" >/dev/null 2>&1; then
        if printf '%s\n' "$DESIRED_DOCKER_CONF" > "$DOCKER_CONF" 2>/dev/null; then
            log "Docker data-root pinned to /data/docker (daemon.json written)"
        else
            warn "Failed to write $DOCKER_CONF"
        fi
    else
        info "Docker daemon.json already correct (/data/docker)"
    fi
else
    warn "/data absent — Docker data-root not pinned (defaults to /var/lib/docker on root)"
fi

# ============================================================================
# Summary
# ============================================================================
echo ""
echo "============================================"
echo "  NE503 AIPC Boot Initialization Complete"
echo "============================================"
echo "  NPU:     ${NPU_DEVICE:-not found}"
echo "  Time:    $(date '+%Y-%m-%d %H:%M:%S %Z')"
echo ""
echo "  Service lifecycle managed by systemd:"
echo "    Enable:   aipc-cli system enable"
echo "    Disable:  aipc-cli system disable"
echo "    Status:   $0 --status"
echo "============================================"
echo ""

log "Boot initialization done"
exit 0
