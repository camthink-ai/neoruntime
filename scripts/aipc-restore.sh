#!/bin/bash
# Restore the AIPC runtime after an OS image replacement.

set -euo pipefail

BACKUP_ROOT="${AIPC_BACKUP_ROOT:-/data/backups/aipc-os-upgrade}"
STATE_ROOT="${AIPC_RESTORE_STATE_ROOT:-/var/lib/aipc-restore}"
CURRENT="${BACKUP_ROOT}/current"
OS_COMPAT_FILE="${AIPC_OS_COMPATIBILITY_FILE:-/etc/aipc-os-release}"
DATA_SCHEMA_FILE="${AIPC_DATA_SCHEMA_FILE:-/data/aipc-data/schema-version}"
MAINTENANCE_MARKER="${AIPC_MAINTENANCE_MARKER:-/run/aipc-maintenance-mode}"

log() { echo "[aipc-restore] $*"; }
fail() {
    log "ERROR: $*"
    mkdir -p "${STATE_ROOT}" 2>/dev/null || true
    printf '%s\n' "$*" >"${STATE_ROOT}/last-error" 2>/dev/null || true
    printf '%s\n' "$*" >"${MAINTENANCE_MARKER}" 2>/dev/null || true
    exit 1
}

require_nonempty() {
    local path="$1"
    [[ -f "$path" && -s "$path" ]] || fail "missing or empty restored file: $path"
}

require_executable() {
    local path="$1"
    [[ -f "$path" && -s "$path" && -x "$path" ]] || \
        fail "missing, empty, or non-executable restored file: $path"
}

validate_restored_release() {
    local file empty
    for file in \
        /data/aipc/app-manifest.json \
        /data/aipc/VERSION \
        /data/aipc/systemd/aipc-platform.target \
        /data/aipc/etc/platform-api.yaml \
        /data/aipc/etc/camera-daemon.yaml; do
        require_nonempty "$file"
    done
    for file in \
        /data/aipc/bin/platform-api \
        /data/aipc/bin/camera-daemon \
        /data/aipc/libexec/aipc-compat-check \
        /data/aipc/libexec/aipc-os-updater \
        /data/aipc/scripts/aipc-install-current-root.sh \
        /data/aipc/scripts/aipc-firstboot.sh; do
        require_executable "$file"
    done
    while IFS= read -r empty; do
        fail "empty immutable restored file: $empty"
    done < <(find /data/aipc/bin /data/aipc/docs /data/aipc/etc \
        /data/aipc/firmware /data/aipc/libexec /data/aipc/recovery \
        /data/aipc/scripts /data/aipc/share /data/aipc/swagger-ui \
        /data/aipc/systemd /data/aipc/web \
        -type f -size 0 -print 2>/dev/null)
}

json_string() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1
}

json_number() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$1" | head -1
}

schema_supported() {
    local manifest="$1" schema="$2" values
    values="$(tr '\n' ' ' < "$manifest" |
        sed -n 's/.*"supported_data_schema"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p' |
        tr ',' ' ')"
    for value in $values; do
        [[ "$value" == "$schema" ]] && return 0
    done
    return 1
}

check_compatibility() {
    local manifest="$1"
    if [[ ! -f "$OS_COMPAT_FILE" ]]; then
        log "WARN: $OS_COMPAT_FILE is absent; OS was never upgraded (legacy image). Skipping restore."
        log "WARN: Stale backup symlink at ${CURRENT} will be removed."
        rm -f "${CURRENT}"
        exit 0
    fi
    [[ -f "$manifest" ]] || fail "APP_MANIFEST_MISSING: $manifest"
    [[ -f "$DATA_SCHEMA_FILE" ]] || fail "APP_DATA_SCHEMA_MISSING: $DATA_SCHEMA_FILE"

    local os_machine os_product os_level os_schema app_machine app_product app_level app_schema current_schema
    os_machine="$(sed -n 's/^MACHINE=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
    os_product="$(sed -n 's/^PRODUCT=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
    os_level="$(sed -n 's/^AIPC_COMPAT_LEVEL=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
    os_schema="$(sed -n 's/^DATA_SCHEMA=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
    app_machine="$(json_string "$manifest" machine)"
    app_product="$(json_string "$manifest" product)"
    app_level="$(json_number "$manifest" required_compat_level)"
    app_schema="$(json_number "$manifest" target_data_schema)"
    current_schema="$(tr -d '[:space:]' < "$DATA_SCHEMA_FILE")"

    [[ -n "$os_machine" && -n "$os_level" && -n "$os_schema" ]] ||
        fail "OS_COMPATIBILITY_METADATA_INVALID"
    [[ "$os_machine" == "$app_machine" ]] ||
        fail "APP_MACHINE_MISMATCH: OS=$os_machine App=$app_machine"
    if [[ -n "$os_product" && -n "$app_product" && "$os_product" != "$app_product" ]]; then
        fail "APP_PRODUCT_MISMATCH: OS=$os_product App=$app_product"
    fi
    [[ "$os_level" == "$app_level" ]] ||
        fail "APP_COMPAT_LEVEL_MISMATCH: OS=$os_level App=$app_level"
    [[ "$os_schema" == "$current_schema" ]] ||
        fail "APP_DATA_SCHEMA_UNSUPPORTED: OS=$os_schema current=$current_schema"
    [[ "$os_schema" == "$app_schema" ]] ||
        fail "APP_DATA_SCHEMA_UNSUPPORTED: OS=$os_schema App target=$app_schema"
    schema_supported "$manifest" "$current_schema" ||
        fail "APP_DATA_SCHEMA_UNSUPPORTED: App does not support schema $current_schema"
}

mkdir -p "${STATE_ROOT}"

if [[ ! -e "${CURRENT}" ]]; then
    log "No upgrade backup selected; no restore needed"
    exit 0
fi

BACKUP_DIR="$(readlink -f "${CURRENT}")"
[[ -d "${BACKUP_DIR}" ]] || fail "current backup target is not a directory"
case "${BACKUP_DIR}/" in
    "${BACKUP_ROOT}/"*) ;;
    *) fail "current backup resolves outside ${BACKUP_ROOT}" ;;
esac

JOB_ID="$(basename "${BACKUP_DIR}")"
DONE_MARKER="${STATE_ROOT}/${JOB_ID}.done"
if [[ -f "${DONE_MARKER}" ]]; then
    log "Backup ${JOB_ID} was already restored on this OS"
    exit 0
fi

[[ -f "${BACKUP_DIR}/manifest.json" ]] || fail "backup manifest is missing"
[[ -f "${BACKUP_DIR}/SHA256SUMS" ]] || fail "backup checksums are missing"
[[ "$(cat "${BACKUP_DIR}/status" 2>/dev/null || true)" == "ready" ]] ||
    fail "backup ${JOB_ID} is not marked ready"

(
    cd "${BACKUP_DIR}"
    sha256sum -c SHA256SUMS
) || fail "backup checksum verification failed"

safe_extract() {
    local archive="$1"
    [[ -f "${BACKUP_DIR}/${archive}" ]] || return 0
    if tar -tzf "${BACKUP_DIR}/${archive}" |
        grep -Eq '(^/|(^|/)\.\.(/|$))'; then
        fail "unsafe path found in ${archive}"
    fi
    log "Restoring ${archive}"
    tar -xzf "${BACKUP_DIR}/${archive}" -C / --no-same-owner
}

safe_extract_to() {
    local archive="$1" destination="$2"
    [[ -f "${BACKUP_DIR}/${archive}" ]] || return 1
    if tar -tzf "${BACKUP_DIR}/${archive}" |
        grep -Eq '(^/|(^|/)\.\.(/|$))'; then
        fail "unsafe path found in ${archive}"
    fi
    mkdir -p "$destination"
    tar -xzf "${BACKUP_DIR}/${archive}" -C "$destination" --no-same-owner
}

restore_regular_file() {
    local root="$1" relative="$2" destination="/$2" mode="${3:-0644}"
    local source="${root}/${relative}"

    [[ -f "$source" && ! -L "$source" ]] || return 1
    mkdir -p "$(dirname "$destination")"
    install -m "$mode" "$source" "$destination"
    return 0
}

restore_path_preserve_type() {
    local root="$1" relative="$2" destination="/$2"
    local source="${root}/${relative}"

    [[ -e "$source" || -L "$source" ]] || return 1
    mkdir -p "$(dirname "$destination")"
    rm -f "$destination"
    cp -aP "$source" "$destination"
    return 0
}

restore_device_config() {
    [[ -f "${BACKUP_DIR}/device-config.tar.gz" ]] || return 0

    local tmp timezone restored_time=0 restored_ca=0
    tmp="$(mktemp -d "${STATE_ROOT}/device-config.XXXXXX")" || fail "create device-config staging directory"
    if ! safe_extract_to device-config.tar.gz "$tmp"; then
        rm -rf "$tmp"
        return 0
    fi

    if restore_regular_file "$tmp" "etc/timezone" 0644; then
        timezone="$(tr -d '\r\n' </etc/timezone 2>/dev/null || true)"
        if [[ -n "$timezone" && -e "/usr/share/zoneinfo/${timezone}" ]]; then
            ln -sfn "/usr/share/zoneinfo/${timezone}" /etc/localtime 2>/dev/null || true
        elif [[ -e "$tmp/etc/localtime" || -L "$tmp/etc/localtime" ]]; then
            restore_path_preserve_type "$tmp" "etc/localtime" || true
        fi
        restored_time=1
    elif [[ -e "$tmp/etc/localtime" || -L "$tmp/etc/localtime" ]]; then
        restore_path_preserve_type "$tmp" "etc/localtime" || true
        restored_time=1
    fi

    if restore_regular_file "$tmp" "etc/systemd/timesyncd.conf" 0644; then
        restored_time=1
    fi
    restore_regular_file "$tmp" "etc/systemd/ntp-units.d/10-aipc-timesyncd.list" 0644 >/dev/null 2>&1 || true
    if [[ $restored_time -eq 1 ]]; then
        log "Restored time and NTP projection"
    fi

    if [[ -f "$tmp/etc/resolv.conf" && ! -L "$tmp/etc/resolv.conf" ]]; then
        if [[ -L /etc/resolv.conf ]]; then
            log "Skipped /etc/resolv.conf restore because the new OS owns it as a symlink"
        else
            restore_regular_file "$tmp" "etc/resolv.conf" 0644 >/dev/null 2>&1 && \
                log "Restored static resolver configuration"
        fi
    fi

    if [[ -d "$tmp/usr/local/share/ca-certificates" ]]; then
        mkdir -p /usr/local/share/ca-certificates
        cp -aP "$tmp/usr/local/share/ca-certificates"/. /usr/local/share/ca-certificates/ 2>/dev/null || true
        restored_ca=1
    fi
    if [[ $restored_ca -eq 1 ]]; then
        update-ca-certificates >/dev/null 2>&1 || true
        log "Restored local CA certificates"
    fi

    if restore_regular_file "$tmp" "home/root/apps/resources/final_calibration.json" 0644; then
        log "Restored field calibration file"
    fi

    rm -rf "$tmp"
}

# Network and remote identity are device-specific. OS-owned systemd, sysctl,
# journald, container runtime and loader configuration always come from the new
# image and must never be overwritten with an old-rootfs backup.
safe_extract network.tar.gz

# In owner=app mode, aipc-restore runs after local-fs.target, meaning
# systemd-networkd might have already started and applied default configurations.
# We must restart the network service to apply the restored static IP immediately.
if systemctl is-active --quiet systemd-networkd 2>/dev/null; then
    systemctl restart systemd-networkd || true
    log "Restarted systemd-networkd to apply restored configuration"
fi

safe_extract ssh.tar.gz
restore_device_config
check_compatibility "/data/aipc/app-manifest.json"
safe_extract aipc.tar.gz
safe_extract runtime.tar.gz
safe_extract systemd.tar.gz
validate_restored_release

# Restore application units, but never let an older backup override the
# OS-owned boot control plane. These vendor units must match the new rootfs.
OS_OWNED_UNITS=(
    aipc-restore.service
    aipc-firstboot.service
    aipc-autostart.service
    aipc-os-verify.service
)
for unit in "${OS_OWNED_UNITS[@]}"; do
    vendor_unit=""
    for vendor_dir in /usr/lib/systemd/system /lib/systemd/system; do
        if [[ -f "${vendor_dir}/${unit}" ]]; then
            vendor_unit="${vendor_dir}/${unit}"
            break
        fi
    done
    [[ -n "$vendor_unit" ]] || continue
    if [[ -e "/etc/systemd/system/${unit}" || -L "/etc/systemd/system/${unit}" ]]; then
        rm -f "/etc/systemd/system/${unit}"
        log "Preserved OS-owned unit: $unit"
    fi
done

# Rebuild /usr/bin symlinks deterministically. The real binaries survive under
# /data/aipc/bin, but /usr/bin lives on the rewritten rootfs and must be
# re-pointed. This replaces extracting real binaries from runtime.tar.gz (that
# archive is still safe_extract'd for backward compat with pre-migration backups).
SERVICES_ORDERED=(
    aipc-healthmon
    event-bus
    camera-daemon
    ai-runtime
    platform-api
    app-manager
    device-control
    device-discovery
)
if [[ -d /data/aipc/bin ]]; then
    for bin in "${SERVICES_ORDERED[@]}" aipc-cli; do
        if [[ -x "/data/aipc/bin/${bin}" ]]; then
            ln -sf "/data/aipc/bin/${bin}" "/usr/bin/${bin}"
        fi
    done
fi

# libexec upgrade tools live under /data/aipc/libexec (persistent) with
# /usr/libexec symlinks. Boot helpers (aipc-restore/firstboot/autostart) remain
# real files baked into /usr/libexec by the image — never symlink those.
for tool in aipc-os-updater aipc-compat-check; do
    if [[ -x "/data/aipc/libexec/${tool}" ]]; then
        ln -sf "/data/aipc/libexec/${tool}" "/usr/libexec/${tool}"
    fi
done

chmod 0700 /home/root/.ssh /root/.ssh 2>/dev/null || true
chmod 0600 /home/root/.ssh/authorized_keys /root/.ssh/authorized_keys 2>/dev/null || true
chmod 0600 /etc/ssh/ssh_host_*_key 2>/dev/null || true
systemctl daemon-reload 2>/dev/null || true
for unit in "${OS_OWNED_UNITS[@]}"; do
    systemctl reenable "$unit" >/dev/null 2>&1 || systemctl enable "$unit" >/dev/null 2>&1 || true
done
ldconfig 2>/dev/null || true

if [[ ! -x /data/aipc/bin/platform-api && ! -x /opt/aipc/bin/platform-api && ! -x /usr/bin/platform-api ]]; then
    fail "restore completed but platform-api is still missing"
fi

printf 'restored_at=%s\nbackup=%s\n' "$(date -u +%FT%TZ)" "${JOB_ID}" >"${DONE_MARKER}"
rm -f "${STATE_ROOT}/last-error"
rm -f "${MAINTENANCE_MARKER}"
sync
log "Backup ${JOB_ID} restored successfully"
