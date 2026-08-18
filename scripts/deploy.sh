#!/bin/bash
#
# AIPC Platform - Hot-swap Deploy Script for Hailo-15
#
# This script is bundled inside the release tarball and runs on the target device.
# It performs a graceful hot-swap: stop services -> update files -> start services,
# with automatic backup and rollback support.
#
# Usage:
#   ./deploy.sh                  # Full deploy (binaries + libs + configs + systemd)
#   ./deploy.sh --no-config      # Deploy without overwriting config files
#   ./deploy.sh --rollback       # Rollback to previous version
#   ./deploy.sh --status         # Show current deployment status
#
set -euo pipefail

FAILURE_DETAIL=""
SERVICES_STOPPED=0
SWAP_ACTIVE=0
DEPLOY_COMMITTED=0
LAST_BACKUP_DIR=""
STAGING_DIR=""
PREVIOUS_DIR=""
PREVIOUS_SCHEMA_FILE=""
PREVIOUS_SCHEMA_PRESENT=0

json_escape() {
    local value="${1:-}"
    value=${value//\\/\\\\}
    value=${value//\"/\\\"}
    value=${value//$'\n'/\\n}
    printf '%s' "$value"
}

record_failure() {
    local message="${1:-}"
    [[ -n "$message" ]] || return 0
    [[ -n "${FAILURE_DETAIL:-}" ]] || FAILURE_DETAIL="$message"
}

write_ota_status_file() {
    local path="$1" status="$2" progress="$3" message="$4" error="$5" reboot_needed="$6"
    [[ -n "$path" ]] || return 0

    mkdir -p "$(dirname "$path")" 2>/dev/null || return 0
    local tmp="${path}.tmp.$$"
    local finished_at=0
    local start_time="${AIPC_OTA_START_TIME:-0}"
    local boot_id=""
    if [[ -r /proc/sys/kernel/random/boot_id ]]; then
        boot_id="$(tr -d '\n' </proc/sys/kernel/random/boot_id 2>/dev/null || true)"
    fi
    case "$status" in
        idle|success|failed) finished_at="$(date +%s)" ;;
    esac
    if printf '{"job_id":"%s","status":"%s","progress":%s,"message":"%s","current_step":"%s","version":"%s","start_time":%s,"finished_at":%s,"error":"%s","reboot_needed":%s,"reboot_confirmed":false,"boot_id":"%s","log_path":"%s"}\n' \
        "$(json_escape "${AIPC_OTA_JOB_ID:-}")" "$(json_escape "$status")" "$progress" "$(json_escape "$message")" \
        "$(json_escape "$status")" "$(json_escape "${AIPC_OTA_VERSION:-unknown}")" \
        "$start_time" "$finished_at" "$(json_escape "$error")" "$reboot_needed" \
        "$(json_escape "$boot_id")" "$(json_escape "${AIPC_OTA_LOG_FILE:-}")" >"$tmp"; then
        mv -f "$tmp" "$path"
    else
        rm -f "$tmp"
    fi
}

write_ota_status() {
    local status="$1" progress="$2" message="$3" error="${4:-}" reboot_needed="${5:-false}"
    local primary="${AIPC_OTA_STATUS_FILE:-}"
    local persisted="${AIPC_OTA_PERSIST_STATUS_FILE:-/data/aipc-data/ota_status.json}"

    write_ota_status_file "$primary" "$status" "$progress" "$message" "$error" "$reboot_needed"
    if [[ "$persisted" != "$primary" ]]; then
        write_ota_status_file "$persisted" "$status" "$progress" "$message" "$error" "$reboot_needed"
    fi
}

ota_reboot_after_success_enabled() {
    case "${AIPC_OTA_REBOOT_AFTER_SUCCESS:-1}" in
        1|true|TRUE|yes|YES|on|ON) return 0 ;;
        *) return 1 ;;
    esac
}

schedule_ota_reboot() {
    ota_reboot_after_success_enabled || return 0

    local delay="${AIPC_OTA_REBOOT_DELAY_SECONDS:-6}"
    [[ "$delay" =~ ^[0-9]+$ ]] || delay=6

    sync || true

    local reboot_script='sync; if command -v systemctl >/dev/null 2>&1; then exec systemctl reboot; elif command -v reboot >/dev/null 2>&1; then exec reboot; else exec /sbin/reboot; fi'
    if command -v systemd-run >/dev/null 2>&1; then
        systemd-run --no-block --collect \
            --unit="aipc-ota-reboot-$$" \
            --description="AIPC OTA post-upgrade reboot" \
            --on-active="${delay}s" \
            /bin/sh -c "$reboot_script" >/dev/null 2>&1 && return 0
    fi

    ( sleep "$delay"; /bin/sh -c "$reboot_script" ) >/dev/null 2>&1 &
}

# Record the exact failing command. The EXIT trap performs rollback and writes
# the final OTA result, including failures caused by an explicit `exit 1`.
err_trap() {
    local rc=$?
    FAILURE_DETAIL="line ${1:-?}, exit=${rc}: ${2:-}"
    if declare -F log >/dev/null 2>&1; then
        err "ERROR: $FAILURE_DETAIL"
    else
        echo "ERROR: $FAILURE_DETAIL" >&2
    fi
}
trap 'err_trap "$LINENO" "$BASH_COMMAND"' ERR

exit_trap() {
    local rc=$?
    trap - ERR EXIT
    set +e

    if (( rc != 0 )); then
        [[ -n "$FAILURE_DETAIL" ]] || FAILURE_DETAIL="deploy exited with status $rc"
        if declare -F rollback_transaction >/dev/null 2>&1; then
            rollback_transaction
        fi
        write_ota_status "failed" 0 "Firmware deployment failed" "$FAILURE_DETAIL"
    elif (( DEPLOY_COMMITTED == 1 )); then
        [[ -z "$PREVIOUS_DIR" ]] || rm -rf "$PREVIOUS_DIR"
        [[ -z "$STAGING_DIR" ]] || rm -rf "$STAGING_DIR"
        if ota_reboot_after_success_enabled; then
            write_ota_status "success" 100 "Firmware upgrade completed; rebooting" "" true
            schedule_ota_reboot
        else
            write_ota_status "success" 100 "Firmware upgrade completed" "" false
        fi
    fi
    exit "$rc"
}
trap exit_trap EXIT

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# ---------- Configuration ----------
# /data/aipc is the sole canonical install root — it lives on the persistent
# /data partition (p3) and survives single-recovery OS upgrades that rewrite
# p2. The install root is fixed and not overridable.
INSTALL_PREFIX="/data/aipc"
INSTALL_BIN="${INSTALL_PREFIX}/bin"
INSTALL_LIB="${INSTALL_PREFIX}/lib/hal"
INSTALL_ETC="${INSTALL_PREFIX}/etc"
INSTALL_LOG="${INSTALL_PREFIX}/logs"
INSTALL_WEB="${INSTALL_PREFIX}/web"
INSTALL_MODELS="${INSTALL_PREFIX}/models"
INSTALL_RECOVERY="${INSTALL_PREFIX}/recovery"
INSTALL_NGINX="${INSTALL_PREFIX}/nginx"
AIPC_NGINX_ROOT="${AIPC_NGINX_ROOT:-/data/nginx}"
SYSTEMD_DIR="/etc/systemd/system"
AIPC_DATA_ROOT="${AIPC_DATA_ROOT:-/data/aipc-data}"
BACKUP_BASE="/data/backups/aipc-app"
VERSION_FILE="${INSTALL_PREFIX}/VERSION"
APP_MANIFEST_FILE="${INSTALL_PREFIX}/app-manifest.json"
DATA_SCHEMA_FILE="${AIPC_DATA_ROOT}/schema-version"
HEALTH_TIMEOUT=60

SERVICES_ORDERED=(
    aipc-healthmon
    event-bus
    camera-daemon
    ai-runtime
    platform-api
    app-manager
    device-control
    device-discovery
    aipc-nginx-gateway
)

# New images declare boot-control ownership in a dedicated marker. Fall back to
# the legacy key for already-deployed owner=os images.
OS_BOOT_CONTROL_OWNER="app"
if grep -qs '^os$' /etc/aipc-bootstrap-owner 2>/dev/null || \
   grep -qs '^AIPC_BOOTSTRAP_OWNER=os$' /etc/aipc-os-release; then
    OS_BOOT_CONTROL_OWNER="os"
fi

# ---------- Colors ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m'

log()  { echo -e "${GREEN}[deploy]${NC} $*"; }
warn() { echo -e "${YELLOW}[deploy]${NC} $*"; }
err()  { echo -e "${RED}[deploy]${NC} $*" >&2; }
info() { echo -e "${CYAN}[deploy]${NC} $*"; }

# ---------- Parse args ----------
MODE="deploy"
DEPLOY_CONFIG=1
FORCE=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --rollback)   MODE="rollback";    shift ;;
        --status)     MODE="status";      shift ;;
        --no-config)  DEPLOY_CONFIG=0;    shift ;;
        --force)      FORCE=1;            shift ;;
        -h|--help)
            cat <<'USAGE'
AIPC Hot-swap Deploy Script

Usage: deploy.sh [OPTIONS]

Modes:
  (default)        Full deployment with hot-swap
  --rollback       Rollback to the previous backup
  --status         Show current version and backup info

Options:
  --no-config      Skip config file deployment (preserve existing configs)
  --force          Skip confirmation prompts
  -h, --help       Show this help
USAGE
            exit 0
            ;;
        *) err "Unknown arg: $1"; exit 1 ;;
    esac
done

# ---------- Root check ----------
if [[ $EUID -ne 0 ]]; then
    err "Must run as root"
    exit 1
fi

# Derived paths. INSTALL_PREFIX is fixed at /data/aipc (no override), so these
# are static — re-listed here for readability and single-source-of-truth editing.
INSTALL_BIN="${INSTALL_PREFIX}/bin"
INSTALL_LIB="${INSTALL_PREFIX}/lib/hal"
INSTALL_ETC="${INSTALL_PREFIX}/etc"
INSTALL_LOG="${INSTALL_PREFIX}/logs"
INSTALL_WEB="${INSTALL_PREFIX}/web"
INSTALL_MODELS="${INSTALL_PREFIX}/models"
INSTALL_RECOVERY="${INSTALL_PREFIX}/recovery"
INSTALL_NGINX="${INSTALL_PREFIX}/nginx"
AIPC_NGINX_ROOT="${AIPC_NGINX_ROOT:-/data/nginx}"
BACKUP_BASE="/data/backups/aipc-app"
VERSION_FILE="${INSTALL_PREFIX}/VERSION"
APP_MANIFEST_FILE="${INSTALL_PREFIX}/app-manifest.json"
DATA_SCHEMA_FILE="${AIPC_DATA_ROOT}/schema-version"

# ---------- Utility functions ----------

os_owns_boot_control() {
    [[ "$OS_BOOT_CONTROL_OWNER" == "os" ]]
}

is_os_boot_unit() {
    case "$1" in
        aipc-restore.service|aipc-firstboot.service|aipc-autostart.service|aipc-os-verify.service) return 0 ;;
        *) return 1 ;;
    esac
}

is_os_boot_helper() {
    case "$1" in
        aipc-restore|aipc-firstboot|aipc-autostart|aipc-os-restore|aipc-os-firstboot|aipc-os-autostart) return 0 ;;
        *) return 1 ;;
    esac
}

unit_exists() {
    systemctl cat "$1" >/dev/null 2>&1
}

copy_file_no_self() {
    local source="$1" dest="$2" mode="${3:-}"

    if [[ -e "$dest" && "$source" -ef "$dest" ]]; then
        [[ -z "$mode" ]] || chmod "$mode" "$dest"
        return 0
    fi

    cp -f "$source" "$dest"
    [[ -z "$mode" ]] || chmod "$mode" "$dest"
}

install_file_no_self() {
    local mode="$1" source="$2" dest="$3"

    if [[ -e "$dest" && "$source" -ef "$dest" ]]; then
        chmod "$mode" "$dest"
        return 0
    fi

    install -m "$mode" "$source" "$dest"
}

require_nonempty_file() {
    local context="$1" path="$2"
    [[ -f "$path" && -s "$path" ]] || {
        local msg="$context validation failed: missing or empty file $path"
        record_failure "$msg"
        err "$msg"
        return 1
    }
}

require_executable_file() {
    local context="$1" path="$2"
    [[ -f "$path" && -s "$path" && -x "$path" ]] || {
        local msg="$context validation failed: missing, empty, or non-executable file $path"
        record_failure "$msg"
        err "$msg"
        return 1
    }
}

require_resolved_nonempty_file() {
    local context="$1" path="$2" resolved
    resolved="$(readlink -f "$path" 2>/dev/null || true)"
    [[ -n "$resolved" && -f "$resolved" && -s "$resolved" ]] || {
        local msg="$context validation failed: missing or empty resolved file $path"
        record_failure "$msg"
        err "$msg"
        return 1
    }
}

validate_no_empty_immutable_files() {
    local context="$1"
    shift
    local failed=0 empty
    local dirs=()

    for dir in "$@"; do
        [[ -d "$dir" ]] && dirs+=("$dir")
    done
    (( ${#dirs[@]} > 0 )) || return 0

    while IFS= read -r empty; do
        local msg="$context validation failed: empty immutable file $empty"
        record_failure "$msg"
        err "$msg"
        failed=1
    done < <(find "${dirs[@]}" -type f -size 0 -print 2>/dev/null)

    (( failed == 0 ))
}

validate_mcu_ota_packages() {
    local context="$1" root="$2"
    local failed=0 pkg size magic
    shopt -s nullglob
    for pkg in "$root"/firmware/mcu/ne503_ota_package_*.bin; do
        size=$(stat -c '%s' "$pkg" 2>/dev/null || echo 0)
        if (( size < 56 )); then
            local msg="$context validation failed: MCU OTA package is too small ($size bytes): $pkg"
            record_failure "$msg"
            err "$msg"
            failed=1
            continue
        fi
        magic="$(dd if="$pkg" bs=4 count=1 2>/dev/null | od -An -tx1 | tr -d ' \n')"
        if [[ "$magic" != "4f544154" ]]; then
            local msg="$context validation failed: MCU OTA package has bad magic ($magic): $pkg"
            record_failure "$msg"
            err "$msg"
            failed=1
        fi
    done
    shopt -u nullglob
    (( failed == 0 ))
}

validate_release_integrity() {
    local root="$1" unit_root="$2" context="$3" metadata_root="${4:-$1}"
    local failed=0 item units

    for item in camera-daemon ai-runtime app-manager event-bus device-control platform-api; do
        require_executable_file "$context" "$root/bin/$item" || failed=1
    done
    for item in aipc-cli aipc-mcu-prep.sh; do
        require_executable_file "$context" "$root/bin/$item" || failed=1
    done
    [[ ! -e "$root/bin/ne503_boot_prep" ]] ||
        require_executable_file "$context" "$root/bin/ne503_boot_prep" || failed=1

    require_resolved_nonempty_file "$context" "$root/lib/hal/libaipc_hal.so" || failed=1
    validate_hal_runtime_deps "$context" "$root" || failed=1

    for item in aipc-firstboot aipc-autostart aipc-restore aipc-os-updater aipc-compat-check aipc-osd-apply; do
        require_executable_file "$context" "$root/libexec/$item" || failed=1
    done
    for item in aipc-firstboot.sh aipc-install-current-root.sh aipc-configure-platform-api-gateway.py aipc-healthmon.sh aipc-logrotate.sh aipc-os-layout-check.sh; do
        require_executable_file "$context" "$root/scripts/$item" || failed=1
    done

    if [[ -d "$root/nginx" ]]; then
        require_nonempty_file "$context" "$root/nginx/conf/nginx.conf" || failed=1
        require_executable_file "$context" "$root/nginx/sbin/aipc-nginx-app-route-sync.py" || failed=1
        if [[ -d "$root/nginx/runtime" ]]; then
            require_executable_file "$context" "$root/nginx/runtime/bin/nginx" || failed=1
            require_executable_file "$context" "$root/nginx/runtime/rootfs/usr/sbin/nginx" || failed=1
        fi
    fi

    require_nonempty_file "$context" "$root/app-manifest.json" || failed=1
    require_nonempty_file "$context" "$metadata_root/VERSION" || failed=1
    for item in camera-daemon.yaml ai-runtime.yaml app-manager.yaml event-bus.yaml platform-api.yaml; do
        require_nonempty_file "$context" "$root/etc/$item" || failed=1
    done
    validate_camera_daemon_config_paths "$context" "$root" || failed=1

    shopt -s nullglob
    units=("$unit_root"/*.service "$unit_root"/*.timer "$unit_root"/*.target)
    shopt -u nullglob
    (( ${#units[@]} > 0 )) || {
        local msg="$context validation failed: canonical systemd unit set is empty: $unit_root"
        record_failure "$msg"
        err "$msg"
        failed=1
    }
    for item in "${units[@]}"; do
        require_nonempty_file "$context" "$item" || failed=1
    done

    validate_no_empty_immutable_files "$context" \
        "$root/bin" "$root/docs" "$root/etc" "$root/firmware" "$root/libexec" \
        "$root/nginx" "$root/recovery" "$root/scripts" "$root/share" "$root/swagger-ui" \
        "$root/systemd" "$root/web" || failed=1
    validate_mcu_ota_packages "$context" "$root" || failed=1

    (( failed == 0 ))
}

yaml_scalar_value() {
    local file="$1" key="$2"
    sed -n -E "s|^[[:space:]]*${key}:[[:space:]]*['\"]?([^'\"#[:space:]]+)['\"]?.*|\\1|p" "$file" | tail -n1
}

medialib_config_is_available() {
    local path="$1" root="${2:-}"
    [[ -f "$path" ]] && return 0
    local rel=""
    if [[ "$path" == /etc/imaging/* ]]; then
        rel="${path#/etc/imaging/}"
    elif [[ "$path" == "$INSTALL_PREFIX"/etc/imaging/* ]]; then
        rel="${path#"$INSTALL_PREFIX"/etc/imaging/}"
    else
        return 1
    fi
    [[ -n "$root" && -f "$root/etc/imaging/$rel" ]] && return 0
    [[ -f "$SCRIPT_DIR/opt/aipc/etc/imaging/$rel" ]] && return 0
    return 1
}

validate_camera_daemon_config_paths() {
    local context="$1"
    local root="$2"
    local cfg="$root/etc/camera-daemon.yaml"
    local failed=0 key got want tmp

    [[ -f "$cfg" ]] || return 0

    declare -A expected=(
        [video_library]="$INSTALL_PREFIX/lib/hal/libaipc_hal.so"
        [codec_library]="$INSTALL_PREFIX/lib/hal/libaipc_hal.so"
        [lens_library]="$INSTALL_PREFIX/lib/hal/libhal-lens-bridge.so"
        [backup_path]="$INSTALL_PREFIX/data/media-backup"
    )
    for key in video_library codec_library lens_library backup_path; do
        got="$(yaml_scalar_value "$cfg" "$key")"
        want="${expected[$key]}"
        if [[ "$got" != "$want" ]]; then
            local msg="$context validation failed: camera-daemon.yaml $key='$got', want '$want'"
            record_failure "$msg"
            err "$msg"
            failed=1
        fi
    done

    # config_path is optional: when absent, camera-daemon falls through to the HAL
    # compiled-in default medialib config (the platform no longer maintains a
    # module-specific path). Validate shape/existence only when explicitly set.
    local media_cfg
    media_cfg="$(yaml_scalar_value "$cfg" config_path)"
    if [[ -n "$media_cfg" ]]; then
        if [[ "$(basename "$media_cfg")" != "webserver_medialib_config.json" ||
              "$(basename "$(dirname "$media_cfg")")" != "medialib_configs" ]]; then
            local msg="$context validation failed: camera-daemon.yaml config_path is not a webserver medialib profile: '$media_cfg'"
            record_failure "$msg"
            err "$msg"
            failed=1
        elif ! medialib_config_is_available "$media_cfg" "$root"; then
            local msg="$context validation failed: camera-daemon.yaml config_path does not exist in /etc/imaging or package payload: '$media_cfg'"
            record_failure "$msg"
            err "$msg"
            failed=1
        fi
    fi

    tmp="/tmp/aipc-camera-config-forbidden.$$"
    if grep -nE '(/opt/aipc|/data/lib/hal|/data/etc|/data/data|ai_example_medialib_config\.json)' "$cfg" >"$tmp" 2>/dev/null; then
        local msg="$context validation failed: camera-daemon.yaml contains legacy/non-canonical paths"
        record_failure "$msg"
        err "$msg"
        sed 's/^/[deploy]   /' "$tmp" >&2
        failed=1
    fi
    rm -f "$tmp"

    (( failed == 0 ))
}

validate_hal_runtime_deps() {
    local context="$1"
    local root="$2"
    local hal="$root/lib/hal/libaipc_hal.so"
    local failed=0 resolved needed lib

    resolved="$(readlink -f "$hal" 2>/dev/null || true)"
    [[ -n "$resolved" && -f "$resolved" ]] || return 0
    command -v readelf >/dev/null 2>&1 || return 0

    needed="$(readelf -d "$resolved" 2>/dev/null |
        sed -n 's/.*Shared library: \[\(libhailodsp\.so[^]]*\)\].*/\1/p' |
        sort -u)"
    [[ -n "$needed" ]] || return 0

    for lib in $needed; do
        if ! { ldconfig -p 2>/dev/null | grep -q "/$lib" ||
            find /lib /usr/lib "$INSTALL_PREFIX/lib" -name "$lib" -type f -print -quit 2>/dev/null | grep -q .; }; then
            local msg="$context validation failed: HAL dependency is missing from current OS: $lib (needed by $resolved)"
            record_failure "$msg"
            err "$msg"
            failed=1
        fi
    done

    (( failed == 0 ))
}

exchange_dirs_with_helper() {
    local left="$1" right="$2" helper candidate
    for candidate in \
        "$INSTALL_PREFIX/libexec/aipc-os-updater" \
        "$PREVIOUS_DIR/libexec/aipc-os-updater" \
        "$STAGING_DIR/libexec/aipc-os-updater" \
        "/usr/libexec/aipc-os-updater"; do
        if [[ -x "$candidate" && -s "$candidate" ]]; then
            helper="$candidate"
            break
        fi
    done
    [[ -n "${helper:-}" ]] || {
        err "No executable aipc-os-updater available for atomic directory exchange"
        return 1
    }
    "$helper" exchange-dirs "$left" "$right"
}

# Boot units owned by the OS image (see is_os_boot_unit) ship their main unit
# file from the rootfs, and deploy.sh must NOT clobber it — otherwise the next
# OS A/B upgrade's vendor unit would be shadowed by a stale /etc copy. But those
# vendor units don't route output to /dev/console, so boot progress is invisible
# on the serial UART. Layer a drop-in instead: it lives in /etc (survives OS A/B
# upgrades), rides on top of whichever vendor unit is active, and overrides only
# the output destinations — an app/debug concern, no OS-owned logic touched.
install_boot_unit_console_dropin() {
    local name="$1" dir
    dir="$SYSTEMD_DIR/$name.d"
    mkdir -p "$dir"
    cat > "$dir/10-console.conf" <<'EOF'
[Service]
StandardOutput=journal+console
StandardError=journal+console
EOF
}

install_systemd_file() {
    local source="$1" name
    name=$(basename "$source")
    if os_owns_boot_control && is_os_boot_unit "$name"; then
        # Remove a legacy /etc override so the vendor unit from the new OS wins,
        # then add a console drop-in so boot output also reaches the serial UART.
        rm -f "$SYSTEMD_DIR/$name"
        install_boot_unit_console_dropin "$name"
        log "  = $name (vendor unit + console drop-in)"
        return 0
    fi
    copy_file_no_self "$source" "$SYSTEMD_DIR/$name" 0644
    log "  + $name -> $SYSTEMD_DIR/"
}

configure_platform_api_gateway_mode() {
    local mode="$1" config="${2:-$INSTALL_ETC/platform-api.yaml}" helper
    for helper in \
        "$INSTALL_PREFIX/scripts/aipc-configure-platform-api-gateway.py" \
        "$SCRIPT_DIR/opt/aipc/scripts/aipc-configure-platform-api-gateway.py" \
        "$SCRIPT_DIR/scripts/aipc-configure-platform-api-gateway.py"; do
        [[ -x "$helper" ]] || continue
        [[ -f "$config" ]] || return 0
        if /usr/bin/python3 "$helper" --config "$config" --mode "$mode" >/dev/null; then
            log "  + platform-api gateway mode: $mode"
        else
            warn "  Failed to configure platform-api gateway mode: $mode"
        fi
        return 0
    done
    warn "  Missing platform-api gateway mode helper"
}

remove_obsolete_nginx_units() {
    local legacy
    for legacy in nginx-data.service aipc-nginx-app-routes.service; do
        systemctl disable --now "$legacy" >/dev/null 2>&1 || true
        rm -f "$SYSTEMD_DIR/$legacy"
    done
}

service_can_be_skipped() {
    case "$1" in
        aipc-nginx-gateway)
            [[ ! -x "$AIPC_NGINX_ROOT/bin/nginx" ]]
            ;;
        *)
            return 1
            ;;
    esac
}

install_nginx_gateway_from_release() {
    local source="$INSTALL_NGINX" file name target
    if [[ ! -d "$source" ]]; then
        log "  Nginx app gateway not packaged; preserving existing nginx config."
        return 0
    fi

    mkdir -p \
        "$AIPC_NGINX_ROOT/bin" "$AIPC_NGINX_ROOT/conf" "$AIPC_NGINX_ROOT/sbin" \
        "$AIPC_NGINX_ROOT/run" "$AIPC_NGINX_ROOT/logs" \
        "$AIPC_NGINX_ROOT/tmp/client_body" "$AIPC_NGINX_ROOT/tmp/proxy" \
        "$AIPC_NGINX_ROOT/tmp/fastcgi" "$AIPC_NGINX_ROOT/tmp/uwsgi" \
        "$AIPC_NGINX_ROOT/tmp/scgi"

    if [[ -d "$source/runtime" ]]; then
        if [[ -d "$source/runtime/bin" ]]; then
            cp -aP "$source/runtime/bin"/. "$AIPC_NGINX_ROOT/bin"/
            chmod 0755 "$AIPC_NGINX_ROOT/bin/nginx" 2>/dev/null || true
            log "  + nginx/bin"
        fi
        if [[ -d "$source/runtime/rootfs" ]]; then
            rm -rf "$AIPC_NGINX_ROOT/rootfs"
            mkdir -p "$AIPC_NGINX_ROOT/rootfs"
            cp -aP "$source/runtime/rootfs"/. "$AIPC_NGINX_ROOT/rootfs"/
            chmod 0755 "$AIPC_NGINX_ROOT/rootfs/usr/sbin/nginx" 2>/dev/null || true
            log "  + nginx/rootfs"
        fi
    fi

    if [[ -d "$source/conf" ]]; then
        for file in "$source"/conf/*; do
            [[ -f "$file" ]] || continue
            name="$(basename -- "$file")"
            target="$AIPC_NGINX_ROOT/conf/$name"
            if [[ "$name" == *.seed ]]; then
                target="$AIPC_NGINX_ROOT/conf/${name%.seed}"
                [[ -s "$target" ]] && continue
            fi
            install_file_no_self 0644 "$file" "$target"
            log "  + nginx/conf/$(basename -- "$target")"
        done
    fi

    if [[ -d "$source/sbin" ]]; then
        for file in "$source"/sbin/*; do
            [[ -f "$file" ]] || continue
            install_file_no_self 0755 "$file" "$AIPC_NGINX_ROOT/sbin/$(basename -- "$file")"
            log "  + nginx/sbin/$(basename -- "$file")"
        done
    fi

    if [[ -x "$AIPC_NGINX_ROOT/bin/nginx" && -x "$AIPC_NGINX_ROOT/sbin/aipc-nginx-app-route-sync.py" ]]; then
        configure_platform_api_gateway_mode nginx "$INSTALL_ETC/platform-api.yaml"
        /usr/bin/python3 "$AIPC_NGINX_ROOT/sbin/aipc-nginx-app-route-sync.py" --ensure-cert >/dev/null 2>&1 || \
            warn "  Initial nginx app route generation failed; the gateway service will retry"
    elif [[ ! -x "$AIPC_NGINX_ROOT/bin/nginx" ]]; then
        warn "  Nginx runtime is absent at $AIPC_NGINX_ROOT/bin/nginx; gateway service will stay skipped"
    fi
}

restore_nginx_gateway_from_backup() {
    local backup_dir="${1:-$LAST_BACKUP_DIR}" entry
    [[ -n "$backup_dir" && -d "$backup_dir/nginx" ]] || return 0
    mkdir -p "$AIPC_NGINX_ROOT"
    for entry in conf sbin bin rootfs; do
        [[ -e "$backup_dir/nginx/$entry" ]] || continue
        rm -rf "$AIPC_NGINX_ROOT/$entry"
        cp -aP "$backup_dir/nginx/$entry" "$AIPC_NGINX_ROOT/$entry"
    done
}

# libexec tools that back the upgrade flow live as real files under the
# persistent install root and are symlinked into /usr/libexec so they survive
# a rootfs rewrite. Boot helpers (restore/firstboot/autostart) stay real files
# baked into /usr/libexec by the image — never symlink those.
is_data_backed_libexec_tool() {
    case "$1" in
        aipc-os-updater|aipc-compat-check|aipc-osd-apply) return 0 ;;
        *) return 1 ;;
    esac
}

install_libexec_file() {
    local source="$1" name
    name=$(basename "$source")
    if os_owns_boot_control && is_os_boot_helper "$name"; then
        log "  = $name (managed by OS image)"
        return 0
    fi
    if is_data_backed_libexec_tool "$name"; then
        install -d "$INSTALL_PREFIX/libexec"
        install_file_no_self 0755 "$source" "$INSTALL_PREFIX/libexec/$name"
        ln -sfn "$INSTALL_PREFIX/libexec/$name" "/usr/libexec/$name"
        log "  + $name -> $INSTALL_PREFIX/libexec/ (+ /usr/libexec symlink)"
        return 0
    fi
    # owner=app keeps /usr/libexec/<name> as a symlink into the staged release
    # under $INSTALL_PREFIX (the canonical indirection — real file on shared /data,
    # symlink in per-slot /usr). cp errors "are the same file" when the dest
    # symlink already resolves to $source (a re-deploy over an existing owner=app
    # slot); refresh the symlink instead so activation stays idempotent. A fresh
    # slot with no symlink (owner=os image bakes real files) still takes cp.
    if [[ -L "/usr/libexec/$name" ]]; then
        ln -sfn "$source" "/usr/libexec/$name"
    else
        copy_file_no_self "$source" "/usr/libexec/$name" 0755
    fi
    log "  + $name -> /usr/libexec/"
}

get_current_version() {
    if [[ -f "$VERSION_FILE" ]]; then
        local ver
        ver=$(grep '^version=' "$VERSION_FILE" 2>/dev/null | cut -d= -f2)
        echo "${ver:-unknown}"
    else
        echo "unknown"
    fi
}

check_package_compatibility() {
    local manifest="$SCRIPT_DIR/opt/aipc/app-manifest.json"
    local os_compat="/etc/aipc-os-release"
    local schema_file="$DATA_SCHEMA_FILE"

    if [[ ! -f "$os_compat" ]]; then
        warn "OS compatibility metadata is absent; allowing one legacy migration deploy"
        return 0
    fi
    [[ -f "$manifest" ]] || {
        err "App package does not contain app-manifest.json"
        return 1
    }

    # Compatibility capabilities are OS-owned. Validate only in deploy mode so
    # diagnostics and emergency rollback remain available on a damaged system.
    local missing_os_fields=()
    grep -Eq '^MACHINE=.+$' "$os_compat" || missing_os_fields+=("MACHINE")
    grep -Eq '^AIPC_COMPAT_LEVEL=[1-9][0-9]*$' "$os_compat" || \
        missing_os_fields+=("AIPC_COMPAT_LEVEL")
    grep -Eq '^DATA_SCHEMA=[1-9][0-9]*$' "$os_compat" || \
        missing_os_fields+=("DATA_SCHEMA")
    if (( ${#missing_os_fields[@]} > 0 )); then
        err "$os_compat is missing or invalid: ${missing_os_fields[*]}"
        return 1
    fi

    local os_machine os_product os_level os_schema app_machine app_product app_level current_schema target_schema
    os_machine=$(sed -n 's/^MACHINE=//p' "$os_compat" | tr -d "\"'" | head -1)
    os_product=$(sed -n 's/^PRODUCT=//p' "$os_compat" | tr -d "\"'" | head -1)
    os_level=$(sed -n 's/^AIPC_COMPAT_LEVEL=//p' "$os_compat" | tr -d "\"'" | head -1)
    os_schema=$(sed -n 's/^DATA_SCHEMA=//p' "$os_compat" | tr -d "\"'" | head -1)
    app_machine=$(sed -n 's/.*"machine"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)
    app_product=$(sed -n 's/.*"product"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$manifest" | head -1)
    app_level=$(sed -n 's/.*"required_compat_level"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$manifest" | head -1)
    target_schema=$(sed -n 's/.*"target_data_schema"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$manifest" | head -1)
    current_schema=$(tr -d '[:space:]' < "$schema_file" 2>/dev/null || echo "$target_schema")

    [[ "$os_machine" == "$app_machine" ]] || {
        err "APP_MACHINE_MISMATCH: OS=$os_machine App=$app_machine"; return 1;
    }
    if [[ -n "$os_product" && -n "$app_product" && "$os_product" != "$app_product" ]]; then
        err "APP_PRODUCT_MISMATCH: OS=$os_product App=$app_product"; return 1
    fi
    [[ "$os_level" == "$app_level" ]] || {
        err "APP_COMPAT_LEVEL_MISMATCH: OS=$os_level App=$app_level"; return 1;
    }
    [[ "$os_schema" == "$current_schema" ]] || {
        err "APP_DATA_SCHEMA_UNSUPPORTED: OS=$os_schema current=$current_schema"; return 1;
    }
    [[ "$os_schema" == "$target_schema" ]] || {
        err "APP_DATA_SCHEMA_UNSUPPORTED: OS=$os_schema App target=$target_schema"; return 1;
    }
    if ! tr '\n' ' ' < "$manifest" |
        sed -n 's/.*"supported_data_schema"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p' |
        tr ',' '\n' | tr -d '[:space:]' | grep -qx "$current_schema"; then
        err "APP_DATA_SCHEMA_UNSUPPORTED: App does not support schema $current_schema"
        return 1
    fi
    log "Compatibility check passed: level=$os_level schema=$current_schema"
}

get_package_version() {
    if [[ -f "$SCRIPT_DIR/VERSION" ]]; then
        grep '^version=' "$SCRIPT_DIR/VERSION" 2>/dev/null | cut -d= -f2
    else
        echo "unknown"
    fi
}

# Detect the sensor without allowing a missing sysfs node or unmatched glob to
# trip `set -e`. The profile itself is a required preflight: failure is reported
# before any service is stopped or installed file is changed.
resolve_sensor_profile() {
    DETECTED_SENSOR="$(grep -hoE 'imx[0-9]+' /sys/class/video4linux/*/name 2>/dev/null | head -n1 || true)"
    DETECTED_CFG=""
    if [[ -n "$DETECTED_SENSOR" ]]; then
        for _soc in hailo15h hailo15l; do
            DETECTED_CFG="$(find "/etc/imaging/cfg/$_soc/$DETECTED_SENSOR" \
                -path '*/medialib_configs/webserver_medialib_config.json' \
                -type f -print -quit 2>/dev/null || true)"
            if [[ -z "$DETECTED_CFG" ]]; then
                local packaged="$SCRIPT_DIR/opt/aipc/etc/imaging/cfg/$_soc/$DETECTED_SENSOR"
                local packaged_cfg
                packaged_cfg="$(find "$packaged" \
                    -path '*/medialib_configs/webserver_medialib_config.json' \
                    -type f -print -quit 2>/dev/null || true)"
                if [[ -n "$packaged_cfg" ]]; then
                    DETECTED_CFG="/etc/imaging/${packaged_cfg#*/etc/imaging/}"
                fi
            fi
            [[ -n "$DETECTED_CFG" ]] && break
        done
    fi
    if [[ -n "$DETECTED_CFG" ]]; then
        log "  Preflight: sensor=$DETECTED_SENSOR -> medialib config $DETECTED_CFG"
    else
        local msg="Preflight failed: no MediaLibrary profile for sensor='${DETECTED_SENSOR:-none}'"
        record_failure "$msg"
        err "$msg"
        return 1
    fi
}

validate_package_preflight() {
    local package_root="$SCRIPT_DIR/opt/aipc"
    local failed=0

    for bin_name in camera-daemon ai-runtime app-manager event-bus device-control platform-api; do
        [[ -x "$package_root/bin/$bin_name" ]] || {
            err "Preflight failed: missing executable $package_root/bin/$bin_name"
            failed=1
        }
    done
    [[ -e "$package_root/lib/hal/libaipc_hal.so" ]] || {
        err "Preflight failed: missing $package_root/lib/hal/libaipc_hal.so"
        failed=1
    }
    [[ -f "$package_root/app-manifest.json" ]] || {
        err "Preflight failed: missing $package_root/app-manifest.json"
        failed=1
    }
    [[ -x "$package_root/libexec/aipc-os-updater" ]] || {
        err "Preflight failed: missing atomic swap helper $package_root/libexec/aipc-os-updater"
        failed=1
    }
    [[ -x "$package_root/libexec/aipc-compat-check" ]] || {
        err "Preflight failed: missing compatibility checker $package_root/libexec/aipc-compat-check"
        failed=1
    }
    [[ -x "$package_root/scripts/aipc-install-current-root.sh" ]] || {
        err "Preflight failed: missing current-root installer"
        failed=1
    }
    [[ -s "$SCRIPT_DIR/systemd/aipc-platform.target" ]] || {
        err "Preflight failed: missing canonical aipc-platform.target"
        failed=1
    }
    [[ -s "$SCRIPT_DIR/VERSION" ]] || {
        err "Preflight failed: missing $SCRIPT_DIR/VERSION"
        failed=1
    }
    for cfg in camera-daemon.yaml ai-runtime.yaml app-manager.yaml event-bus.yaml platform-api.yaml; do
        [[ -s "$package_root/etc/$cfg" ]] || {
            err "Preflight failed: missing config $package_root/etc/$cfg"
            failed=1
        }
    done
    validate_release_integrity "$package_root" "$SCRIPT_DIR/systemd" "package preflight" "$SCRIPT_DIR" || failed=1

    if grep -Rns --include='*.yaml' '/opt/aipc' "$package_root/etc" >"/tmp/aipc-deploy-noncanonical.$$" 2>/dev/null; then
        local msg="Preflight failed: release YAML still contains non-canonical /opt/aipc paths"
        record_failure "$msg"
        err "$msg"
        sed 's/^/[deploy]   /' "/tmp/aipc-deploy-noncanonical.$$" >&2
        failed=1
    fi
    rm -f "/tmp/aipc-deploy-noncanonical.$$"

    resolve_sensor_profile || failed=1
    (( failed == 0 ))
}

stop_services() {
    log "Stopping services (reverse order)..."
    local svcs=("${SERVICES_ORDERED[@]}")
    local i
    for (( i=${#svcs[@]}-1; i>=0; i-- )); do
        local svc="${svcs[$i]}"
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            log "  Stopping $svc ..."
            systemctl stop "$svc" --no-block 2>/dev/null || true
        fi
    done
    sleep 2
    for (( i=${#svcs[@]}-1; i>=0; i-- )); do
        local svc="${svcs[$i]}"
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            warn "  Force killing $svc ..."
            systemctl kill -s SIGKILL "$svc" 2>/dev/null || true
        fi
    done
    log "  All services stopped."
}

start_services() {
    log "Starting services (dependency order)..."
    systemctl daemon-reload

    # Bootstrap infrastructure first, then use the same autostart path as boot.
    if unit_exists aipc-firstboot.service; then
        systemctl enable aipc-firstboot 2>/dev/null || true
        if systemctl start aipc-firstboot 2>/dev/null; then
            log "  Started aipc-firstboot (boot bootstrap: sysctl/core_pattern/journal)"
        else
            warn "  Failed to start aipc-firstboot (sysctl/journal bootstrap may be incomplete)"
        fi
    else
        warn "  aipc-firstboot.service missing — boot bootstrap will not run on reboot"
    fi

    if unit_exists aipc-autostart.service && systemctl enable aipc-autostart.service 2>/dev/null &&
        systemctl restart aipc-autostart.service 2>/dev/null; then
        log "  AIPC services queued by aipc-autostart"
    else
        # Compatibility fallback for installations created before autostart.
        for svc in "${SERVICES_ORDERED[@]}"; do
            unit_exists "$svc.service" || continue
            systemctl enable "$svc" 2>/dev/null || true
            systemctl start "$svc" 2>/dev/null || warn "  Failed to start $svc"
        done
    fi

    if unit_exists aipc-logrotate.timer; then
        systemctl enable --now aipc-logrotate.timer 2>/dev/null \
            && log "  Enabled aipc-logrotate.timer (rotates Go logs every 10 min)" \
            || warn "  Failed to enable aipc-logrotate.timer"
    fi
}

health_check() {
    log "Running health checks (timeout ${HEALTH_TIMEOUT}s)..."
    local all_ok=1
    local deadline=$((SECONDS + HEALTH_TIMEOUT))

    if [[ -x /usr/libexec/aipc-compat-check ]]; then
        if ! /usr/libexec/aipc-compat-check; then
            err "Installed App compatibility check failed"
            all_ok=0
        fi
    else
        err "Missing /usr/libexec/aipc-compat-check"
        all_ok=0
    fi
    validate_release_integrity "$INSTALL_PREFIX" "$INSTALL_PREFIX/systemd" "active release" || all_ok=0
    for data_dir in "$AIPC_DATA_ROOT" "$AIPC_DATA_ROOT/database" "$AIPC_DATA_ROOT/models" "$AIPC_DATA_ROOT/apps"; do
        if [[ ! -d "$data_dir" || ! -r "$data_dir" || ! -w "$data_dir" ]]; then
            err "Persistent data directory is unavailable: $data_dir"
            all_ok=0
        fi
    done

    while [[ $SECONDS -lt $deadline ]]; do
        local services_ok=1
        for svc in "${SERVICES_ORDERED[@]}"; do
            if [[ ! -f "$SYSTEMD_DIR/${svc}.service" ]]; then
                continue
            fi
            if ! systemctl is-active --quiet "$svc" 2>/dev/null; then
                service_can_be_skipped "$svc" && continue
                services_ok=0
                break
            fi
        done
        if [[ $services_ok -eq 1 ]]; then
            break
        fi
        sleep 1
    done

    # platform-api reconciles config_items after startup. Re-check the active
    # camera config after services settle so a stale desired-state row cannot
    # silently rewrite camera-daemon.yaml back to an older install root.
    validate_camera_daemon_config_paths "active release post-reconcile" "$INSTALL_PREFIX" || all_ok=0
    if unit_exists camera-daemon.service; then
        local cam_active cam_sub
        cam_active="$(systemctl show -p ActiveState camera-daemon 2>/dev/null | cut -d= -f2)"
        cam_sub="$(systemctl show -p SubState camera-daemon 2>/dev/null | cut -d= -f2)"
        if [[ "$cam_active" != "active" || "$cam_sub" != "running" ]]; then
            err "camera-daemon is not stably running after health wait (ActiveState=$cam_active SubState=$cam_sub)"
            all_ok=0
        fi
        if journalctl -u camera-daemon -b --since "30 seconds ago" --no-pager 2>/dev/null |
            grep -Eq 'dlopen\(.*libaipc_hal|Failed to load HAL|Failed to initialize camera daemon'; then
            err "camera-daemon logged HAL/init failures during deploy health check"
            all_ok=0
        fi
    fi

    log ""
    log "Service status:"
    for svc in "${SERVICES_ORDERED[@]}"; do
        if [[ ! -f "$SYSTEMD_DIR/${svc}.service" ]]; then
            continue
        fi
        local status
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            status="${GREEN}active${NC}"
        elif service_can_be_skipped "$svc"; then
            status="${YELLOW}skipped${NC}"
        else
            status="${RED}failed${NC}"
            all_ok=0
        fi
        log "  $svc: $status"
    done
    log ""

    return $( [[ $all_ok -eq 1 ]] && echo 0 || echo 1 )
}

migrate_persistent_dir() {
    local source="$1" destination="$2"
    local fail_target
    mkdir -p "$destination"

    if [[ -L "$source" ]]; then
        if [[ "$(readlink -f "$source")" == "$(readlink -f "$destination")" ]]; then
            return 0
        fi
        fail_target="$(readlink -f "$source" 2>/dev/null || true)"
        [[ -z "$fail_target" ]] || cp -a "$fail_target"/. "$destination"/ 2>/dev/null || true
        rm -f "$source"
    elif [[ -d "$source" ]]; then
        cp -a "$source"/. "$destination"/
        rm -rf "$source"
    elif [[ -e "$source" ]]; then
        err "Cannot migrate persistent path because it is not a directory: $source"
        return 1
    fi

    mkdir -p "$(dirname "$source")"
    ln -s "$destination" "$source"
    log "  Persistent path: $source -> $destination"
}

migrate_persistent_data() {
    log "Migrating mutable AIPC data to $AIPC_DATA_ROOT ..."
    mkdir -p "$AIPC_DATA_ROOT"
    migrate_persistent_dir "$INSTALL_PREFIX/data" "$AIPC_DATA_ROOT/database"
    migrate_persistent_dir "$INSTALL_PREFIX/models" "$AIPC_DATA_ROOT/models"
    migrate_persistent_dir "$INSTALL_PREFIX/apps" "$AIPC_DATA_ROOT/apps"
    mkdir -p "$AIPC_DATA_ROOT/containerd"
}

# Build a complete candidate root on the same filesystem as /data/aipc. The
# complete root (including configs and metadata) is validated before rename,
# so a power loss can expose either the old release or the new release, never
# a mixture of both.
prepare_staging() {
    STAGING_DIR="/data/.aipc-stage-$$"
    PREVIOUS_DIR="/data/.aipc-previous-$$"
    rm -rf "$STAGING_DIR" "$PREVIOUS_DIR"
    mkdir -p "$STAGING_DIR"

    if [[ -d "$INSTALL_PREFIX" ]]; then
        cp -a "$INSTALL_PREFIX"/. "$STAGING_DIR"/
    fi

    local package_root="$SCRIPT_DIR/opt/aipc"
    # firmware (MCU OTA packages) and docs (design refs pointed at by unit
    # Documentation=) are staged by the Makefile under opt/aipc/ and must be
    # carried into the new root here, or /data/aipc/firmware/mcu and
    # /data/aipc/docs would be absent after the atomic swap.
    for dir in bin lib/hal libexec scripts recovery web swagger-ui firmware docs share nginx; do
        [[ -d "$package_root/$dir" ]] || continue
        rm -rf "$STAGING_DIR/$dir"
        mkdir -p "$STAGING_DIR/$dir"
        cp -a "$package_root/$dir"/. "$STAGING_DIR/$dir"/
    done

    # Canonical units are immutable release content too. Stage the exact set
    # before the directory exchange so a power loss cannot leave new binaries
    # paired with the previous release's boot contract.
    rm -rf "$STAGING_DIR/systemd"
    mkdir -p "$STAGING_DIR/systemd"
    local unit
    for unit in "$SCRIPT_DIR"/systemd/*.service "$SCRIPT_DIR"/systemd/*.timer "$SCRIPT_DIR"/systemd/*.target; do
        [[ -f "$unit" ]] || continue
        install -m 0644 "$unit" "$STAGING_DIR/systemd/"
    done

    mkdir -p "$STAGING_DIR/etc" "$STAGING_DIR/logs"
    if (( DEPLOY_CONFIG == 1 )); then
        rm -f "$STAGING_DIR"/etc/*.yaml
        for cfg in "$package_root"/etc/*.yaml; do
            [[ -f "$cfg" ]] || continue
            cp -f "$cfg" "$STAGING_DIR/etc/"
        done
    fi
    # Package-owned rootfs configuration (journald/systemd/sysctl/security) is
    # part of the current-root contract. Keep it under /data/aipc/etc so a hot
    # deploy, OS slot switch, or factory/current-root rebuild can re-apply the
    # same drop-ins to /etc. Do not replace the whole etc/ tree: it also holds
    # mutable runtime files such as SSL keys and last-known-time.json.
    for dir in systemd sysctl.d security; do
        [[ -d "$package_root/etc/$dir" ]] || continue
        rm -rf "$STAGING_DIR/etc/$dir"
        mkdir -p "$STAGING_DIR/etc/$dir"
        cp -a "$package_root/etc/$dir"/. "$STAGING_DIR/etc/$dir"/
    done
    [[ -f "$package_root/app-manifest.json" ]] && \
        cp -f "$package_root/app-manifest.json" "$STAGING_DIR/app-manifest.json"
    if [[ -f "$SCRIPT_DIR/VERSION" ]]; then
        cp -f "$SCRIPT_DIR/VERSION" "$STAGING_DIR/VERSION"
        printf 'deploy_time=%s\n' "$(date '+%Y-%m-%d %H:%M:%S')" >>"$STAGING_DIR/VERSION"
    fi

    # Mutable state is external to the release root. Preserve only symlinks in
    # the staged tree so the atomic rename cannot roll database/model state back.
    for mapping in \
        "data:$AIPC_DATA_ROOT/database" \
        "models:$AIPC_DATA_ROOT/models" \
        "apps:$AIPC_DATA_ROOT/apps"; do
        local name="${mapping%%:*}" destination="${mapping#*:}"
        rm -rf "$STAGING_DIR/$name"
        ln -s "$destination" "$STAGING_DIR/$name"
    done

    # config_path is no longer platform-maintained: camera-daemon falls back to the
    # HAL compiled-in default medialib config when it is absent. Proactively strip
    # any config_path the staged yaml still declares (e.g. a release built from an
    # older configs/ tree, or a device that carried one forward) so installs converge
    # on the default instead of retaining a stale module-specific path. backup_path
    # and the explanatory comment lines are left untouched.
    if [[ -f "$STAGING_DIR/etc/camera-daemon.yaml" ]] &&
       grep -qE '^[[:space:]]*config_path:' "$STAGING_DIR/etc/camera-daemon.yaml"; then
        sed -i -E '/^[[:space:]]*config_path:[[:space:]]*.*/d' \
            "$STAGING_DIR/etc/camera-daemon.yaml"
        log "  Stripped legacy media.config_path from staged camera-daemon.yaml"
    fi
    log "  Complete release staged at $STAGING_DIR"
}

validate_staging() {
    log "Validating staged release..."
    local failed=0
    for bin_name in camera-daemon ai-runtime app-manager event-bus device-control platform-api; do
        [[ -x "$STAGING_DIR/bin/$bin_name" ]] || {
            err "Validation failed: missing executable $STAGING_DIR/bin/$bin_name"
            failed=1
        }
    done
    [[ -e "$STAGING_DIR/lib/hal/libaipc_hal.so" ]] || {
        err "Validation failed: missing staged HAL entrypoint"
        failed=1
    }
    local resolved_hal
    resolved_hal="$(readlink -f "$STAGING_DIR/lib/hal/libaipc_hal.so" 2>/dev/null || true)"
    [[ -n "$resolved_hal" && -f "$resolved_hal" ]] || {
        err "Validation failed: staged HAL symlink chain is broken"
        failed=1
    }
    for cfg in camera-daemon.yaml ai-runtime.yaml app-manager.yaml event-bus.yaml platform-api.yaml; do
        [[ -s "$STAGING_DIR/etc/$cfg" ]] || {
            err "Validation failed: missing config $STAGING_DIR/etc/$cfg"
            failed=1
        }
    done
    if grep -Rns --include='*.yaml' '/opt/aipc' "$STAGING_DIR/etc" >"/tmp/aipc-stage-noncanonical.$$" 2>/dev/null; then
        local msg="Validation failed: staged YAML contains /opt/aipc"
        record_failure "$msg"
        err "$msg"
        sed 's/^/[deploy]   /' "/tmp/aipc-stage-noncanonical.$$" >&2
        failed=1
    fi
    rm -f "/tmp/aipc-stage-noncanonical.$$"
    [[ -s "$STAGING_DIR/app-manifest.json" ]] || {
        err "Validation failed: staged app-manifest.json is missing"
        failed=1
    }
    [[ -s "$STAGING_DIR/VERSION" ]] || {
        err "Validation failed: staged VERSION is missing"
        failed=1
    }
    [[ -x "$STAGING_DIR/scripts/aipc-install-current-root.sh" ]] || {
        err "Validation failed: persistent current-root installer is missing"
        failed=1
    }
    [[ -x "$STAGING_DIR/libexec/aipc-compat-check" ]] || {
        err "Validation failed: persistent compatibility checker is missing"
        failed=1
    }
    [[ -s "$STAGING_DIR/systemd/aipc-platform.target" ]] || {
        err "Validation failed: canonical aipc-platform.target is missing"
        failed=1
    }
    validate_release_integrity "$STAGING_DIR" "$STAGING_DIR/systemd" "staged release" || failed=1

    local staged_updater="$STAGING_DIR/libexec/aipc-os-updater" recovery_manifest="$STAGING_DIR/recovery/manifest.json" recovery_error
    if [[ -x "$staged_updater" && -s "$recovery_manifest" ]]; then
        if ! recovery_error=$("$staged_updater" -recovery-dir "$STAGING_DIR/recovery" check-recovery 2>&1); then
            recovery_error="${recovery_error:-cannot execute staged updater}"
            err "Validation failed: staged recovery checker: $recovery_error"
            FAILURE_DETAIL="staged recovery validation failed: $recovery_error"
            failed=1
        fi
    fi
    if (( failed != 0 )); then
        [[ -n "$FAILURE_DETAIL" ]] || FAILURE_DETAIL="staged release validation failed"
        return 1
    fi
}

refresh_runtime_links() {
    local f local_name
    mkdir -p /usr/bin /usr/libexec /etc/ld.so.conf.d
    for f in "$INSTALL_BIN"/*; do
        [[ -f "$f" ]] || continue
        local_name=$(basename "$f")
        [[ "$local_name" =~ \.py$ ]] || ln -sfn "$f" "/usr/bin/$local_name"
    done
    for local_name in aipc-os-updater aipc-compat-check aipc-osd-apply; do
        [[ -x "$INSTALL_PREFIX/libexec/$local_name" ]] && \
            ln -sfn "$INSTALL_PREFIX/libexec/$local_name" "/usr/libexec/$local_name"
    done
    printf '%s\n' "$INSTALL_LIB" >/etc/ld.so.conf.d/aipc.conf
    ldconfig 2>/dev/null || true
}

atomic_release_swap() {
    [[ -d "$STAGING_DIR" ]] || return 1
    if [[ -d "$INSTALL_PREFIX" ]]; then
        exchange_dirs_with_helper "$INSTALL_PREFIX" "$STAGING_DIR"
        # After RENAME_EXCHANGE, the canonical path is the complete new root
        # and the former root is retained at the old staging path.
        PREVIOUS_DIR="$STAGING_DIR"
        STAGING_DIR=""
    else
        mv "$STAGING_DIR" "$INSTALL_PREFIX"
        STAGING_DIR=""
    fi
    SWAP_ACTIVE=1
    refresh_runtime_links
    log "  Atomic release switch complete; previous root retained until health check passes"
}

restore_system_integration() {
    [[ -n "$LAST_BACKUP_DIR" && -d "$LAST_BACKUP_DIR" ]] || return 0
    local file
    for file in "$LAST_BACKUP_DIR"/systemd/*.service "$LAST_BACKUP_DIR"/systemd/*.timer "$LAST_BACKUP_DIR"/systemd/*.target; do
        [[ -f "$file" ]] || continue
        cp -f "$file" "$SYSTEMD_DIR/"
    done
    for file in "$LAST_BACKUP_DIR"/libexec/aipc-*; do
        [[ -f "$file" ]] || continue
        cp -f "$file" /usr/libexec/
        chmod 0755 "/usr/libexec/$(basename "$file")"
    done
    restore_nginx_gateway_from_backup "$LAST_BACKUP_DIR"
    systemctl daemon-reload 2>/dev/null || true
}

rollback_transaction() {
    if (( SWAP_ACTIVE == 1 )) && [[ -d "$PREVIOUS_DIR" ]]; then
        if ! exchange_dirs_with_helper "$INSTALL_PREFIX" "$PREVIOUS_DIR" 2>/dev/null; then
            err "Automatic rollback exchange failed; both release roots were preserved"
            SERVICES_STOPPED=0
            return 1
        fi
        rm -rf "$PREVIOUS_DIR" 2>/dev/null
        SWAP_ACTIVE=0
        refresh_runtime_links
        if [[ -n "$PREVIOUS_SCHEMA_FILE" ]]; then
            if (( PREVIOUS_SCHEMA_PRESENT == 1 )); then
                mkdir -p "$(dirname "$DATA_SCHEMA_FILE")"
                cp -a "$PREVIOUS_SCHEMA_FILE" "$DATA_SCHEMA_FILE"
            else
                rm -f "$DATA_SCHEMA_FILE"
            fi
            rm -f "$PREVIOUS_SCHEMA_FILE"
            PREVIOUS_SCHEMA_FILE=""
        fi
        if [[ -x "$INSTALL_PREFIX/scripts/aipc-install-current-root.sh" ]]; then
            AIPC_ACTIVATE=0 "$INSTALL_PREFIX/scripts/aipc-install-current-root.sh" || \
                restore_system_integration
        else
            restore_system_integration
        fi
        restore_nginx_gateway_from_backup "$LAST_BACKUP_DIR"
        warn "Rolled back to the previous complete release"
    elif (( SWAP_ACTIVE == 1 )); then
        rm -rf "$INSTALL_PREFIX" 2>/dev/null
        SWAP_ACTIVE=0
    fi
    [[ -z "$STAGING_DIR" ]] || rm -rf "$STAGING_DIR"
    [[ -z "$PREVIOUS_SCHEMA_FILE" ]] || rm -f "$PREVIOUS_SCHEMA_FILE"
    if (( SERVICES_STOPPED == 1 )); then
        start_services || true
        SERVICES_STOPPED=0
    fi
}

create_backup() {
    local cur_ver
    cur_ver=$(get_current_version)
    local backup_dir="$BACKUP_BASE/${cur_ver}-$(date +%Y%m%d-%H%M%S)"
    LAST_BACKUP_DIR="$backup_dir"

    log "Creating backup at $backup_dir ..."
    mkdir -p "$backup_dir"/{bin,lib/hal,libexec,etc,systemd,recovery,metadata,nginx}

    # Snapshot every immutable release component, including scripts, libexec,
    # canonical units and future top-level additions. Mutable state is kept in
    # /data/aipc-data and represented by links so backups remain bounded.
    if [[ -d "$INSTALL_PREFIX" ]]; then
        mkdir -p "$backup_dir/release" "$backup_dir/release/logs"
        local entry name
        shopt -s dotglob nullglob
        for entry in "$INSTALL_PREFIX"/*; do
            name="$(basename -- "$entry")"
            case "$name" in data|models|apps|logs) continue ;; esac
            cp -a "$entry" "$backup_dir/release/"
        done
        shopt -u dotglob nullglob
        ln -s "$AIPC_DATA_ROOT/database" "$backup_dir/release/data"
        ln -s "$AIPC_DATA_ROOT/models" "$backup_dir/release/models"
        ln -s "$AIPC_DATA_ROOT/apps" "$backup_dir/release/apps"
    fi

    for svc in "${SERVICES_ORDERED[@]}"; do
        [[ -f "$INSTALL_BIN/$svc" ]]   && cp -a "$INSTALL_BIN/$svc"   "$backup_dir/bin/" 2>/dev/null || true
    done
    for tool in shm-reader nv12-to-jpeg aipc-cli; do
        [[ -f "$INSTALL_BIN/$tool" ]]  && cp -a "$INSTALL_BIN/$tool"  "$backup_dir/bin/" 2>/dev/null || true
    done

    cp -a "$INSTALL_LIB"/libaipc_hal*.so* "$INSTALL_LIB"/libhal-*.so* "$backup_dir/lib/hal/" 2>/dev/null || true
    cp -a "$INSTALL_ETC"/*.yaml       "$backup_dir/etc/"     2>/dev/null || true
    cp -a "$INSTALL_RECOVERY"/*       "$backup_dir/recovery/" 2>/dev/null || true
    for helper in /usr/libexec/aipc-*; do
        [[ -f "$helper" ]] || continue
        if os_owns_boot_control && is_os_boot_helper "$(basename "$helper")"; then
            continue
        fi
        cp -a "$helper" "$backup_dir/libexec/" 2>/dev/null || true
    done

    for unit in "$SYSTEMD_DIR"/aipc-*.service "$SYSTEMD_DIR"/aipc-*.timer "$SYSTEMD_DIR"/aipc-*.target; do
        [[ -f "$unit" ]] || continue
        if os_owns_boot_control && is_os_boot_unit "$(basename "$unit")"; then
            continue
        fi
        cp -a "$unit" "$backup_dir/systemd/" 2>/dev/null || true
    done
    for svc in "${SERVICES_ORDERED[@]}"; do
        [[ -f "$SYSTEMD_DIR/${svc}.service" ]] &&
            cp -a "$SYSTEMD_DIR/${svc}.service" "$backup_dir/systemd/" 2>/dev/null || true
    done

    [[ -f "$VERSION_FILE" ]] && cp -a "$VERSION_FILE" "$backup_dir/"
    [[ -f "$APP_MANIFEST_FILE" ]] && cp -a "$APP_MANIFEST_FILE" "$backup_dir/metadata/"
    [[ -f "$DATA_SCHEMA_FILE" ]] && cp -a "$DATA_SCHEMA_FILE" "$backup_dir/metadata/schema-version"
    if [[ -d "$AIPC_NGINX_ROOT" ]]; then
        for entry in conf sbin bin rootfs; do
            [[ -e "$AIPC_NGINX_ROOT/$entry" ]] || continue
            cp -aP "$AIPC_NGINX_ROOT/$entry" "$backup_dir/nginx/"
        done
    fi

    echo "$backup_dir" > "$BACKUP_BASE/latest"
    log "  Backup complete: $backup_dir"
}

get_latest_backup() {
    if [[ -f "$BACKUP_BASE/latest" ]]; then
        cat "$BACKUP_BASE/latest"
    else
        echo ""
    fi
}

# ==========================================================
#  MODE: status
# ==========================================================
if [[ "$MODE" == "status" ]]; then
    info "============================================"
    info "  AIPC Deployment Status"
    info "============================================"
    info "  Current version: $(get_current_version)"
    if [[ -f "$VERSION_FILE" ]]; then
        info "  Version file:"
        while IFS= read -r line; do
            info "    $line"
        done < "$VERSION_FILE"
    fi
    info ""
    info "  Services:"
    for svc in "${SERVICES_ORDERED[@]}"; do
        if systemctl is-active --quiet "$svc" 2>/dev/null; then
            info "    $svc: ${GREEN}active${NC}"
        elif systemctl is-enabled --quiet "$svc" 2>/dev/null; then
            info "    $svc: ${RED}inactive${NC}"
        else
            info "    $svc: ${YELLOW}not installed${NC}"
        fi
    done
    info ""
    if [[ -d "$BACKUP_BASE" ]]; then
        info "  Backups:"
        ls -1d "$BACKUP_BASE"/*/  2>/dev/null | while read -r d; do
            info "    $(basename "$d")"
        done
        latest=""
        latest=$(get_latest_backup)
        [[ -n "$latest" ]] && info "  Latest backup: $(basename "$latest")"
    else
        info "  Backups: none"
    fi
    info "============================================"
    exit 0
fi

# ==========================================================
#  MODE: rollback
# ==========================================================
if [[ "$MODE" == "rollback" ]]; then
    BACKUP_DIR=$(get_latest_backup)
    if [[ -z "$BACKUP_DIR" || ! -d "$BACKUP_DIR" ]]; then
        err "No backup found to rollback to."
        exit 1
    fi

    log "============================================"
    log "  Rolling back to: $(basename "$BACKUP_DIR")"
    log "============================================"

    if [[ $FORCE -eq 0 ]]; then
        read -rp "Continue rollback? [y/N] " confirm
        [[ "$confirm" =~ ^[Yy]$ ]] || { log "Aborted."; exit 0; }
    fi

    stop_services
    SERVICES_STOPPED=1

    # New backups contain a complete immutable release. Restore it with the
    # same atomic exchange used by deploy, retaining the current root until the
    # restored release passes compatibility and health checks.
    if [[ -d "$BACKUP_DIR/release" && \
          -x "$BACKUP_DIR/release/libexec/aipc-os-updater" && \
          -x "$BACKUP_DIR/release/scripts/aipc-install-current-root.sh" ]]; then
        rollback_stage="/data/.aipc-manual-rollback-$$"
        rm -rf "$rollback_stage"
        cp -a "$BACKUP_DIR/release" "$rollback_stage"
        PREVIOUS_SCHEMA_FILE="/data/.aipc-schema-previous-$$"
        rm -f "$PREVIOUS_SCHEMA_FILE"
        if [[ -f "$DATA_SCHEMA_FILE" ]]; then
            cp -a "$DATA_SCHEMA_FILE" "$PREVIOUS_SCHEMA_FILE"
            PREVIOUS_SCHEMA_PRESENT=1
        fi
        STAGING_DIR="$rollback_stage"
        exchange_dirs_with_helper "$INSTALL_PREFIX" "$rollback_stage"
        STAGING_DIR=""
        PREVIOUS_DIR="$rollback_stage"
        SWAP_ACTIVE=1
        if [[ -f "$BACKUP_DIR/metadata/schema-version" ]]; then
            mkdir -p "$(dirname "$DATA_SCHEMA_FILE")"
            cp -a "$BACKUP_DIR/metadata/schema-version" "$DATA_SCHEMA_FILE"
        fi
        refresh_runtime_links
        validate_release_integrity "$INSTALL_PREFIX" "$INSTALL_PREFIX/systemd" "rollback active release"
        AIPC_ACTIVATE=0 "$INSTALL_PREFIX/scripts/aipc-install-current-root.sh"
        restore_nginx_gateway_from_backup "$BACKUP_DIR"
        start_services

        if health_check; then
            rm -rf "$PREVIOUS_DIR"
            PREVIOUS_DIR=""
            SWAP_ACTIVE=0
            SERVICES_STOPPED=0
            rm -f "$PREVIOUS_SCHEMA_FILE"
            PREVIOUS_SCHEMA_FILE=""
            log "Rollback successful."
            exit 0
        fi
        FAILURE_DETAIL="line $LINENO, exit=1: rollback health_check"
        err "Rollback candidate failed health checks; restoring the current release"
        exit 1
    fi

    warn "Legacy partial backup detected; using compatibility rollback path"
    log "Restoring binaries..."
    cp -a "$BACKUP_DIR/bin"/* "$INSTALL_BIN/" 2>/dev/null || true

    log "Restoring libraries..."
    cp -aP "$BACKUP_DIR/lib/hal"/* "$INSTALL_LIB/" 2>/dev/null || true

    log "Restoring configs..."
    cp -a "$BACKUP_DIR/etc"/* "$INSTALL_ETC/" 2>/dev/null || true
    cp -a "$BACKUP_DIR/recovery"/* "$INSTALL_RECOVERY/" 2>/dev/null || true

    log "Restoring systemd units..."
    for file in "$BACKUP_DIR/systemd"/*.service "$BACKUP_DIR/systemd"/*.timer "$BACKUP_DIR/systemd"/*.target; do
        [[ -f "$file" ]] || continue
        install_systemd_file "$file"
    done
    for file in "$BACKUP_DIR/libexec"/aipc-*; do
        [[ -f "$file" ]] || continue
        install_libexec_file "$file"
    done
    restore_nginx_gateway_from_backup "$BACKUP_DIR"

    [[ -f "$BACKUP_DIR/VERSION" ]] && cp -a "$BACKUP_DIR/VERSION" "$VERSION_FILE"
    [[ -f "$BACKUP_DIR/metadata/app-manifest.json" ]] &&
        cp -a "$BACKUP_DIR/metadata/app-manifest.json" "$APP_MANIFEST_FILE"
    if [[ -f "$BACKUP_DIR/metadata/schema-version" ]]; then
        mkdir -p "$(dirname "$DATA_SCHEMA_FILE")"
        cp -a "$BACKUP_DIR/metadata/schema-version" "$DATA_SCHEMA_FILE"
    fi

    ldconfig 2>/dev/null || true
    start_services
    SERVICES_STOPPED=0

    if health_check; then
        log "Rollback successful."
    else
        err "Rollback completed but some services failed. Check: journalctl -u '<service>' -n 50"
    fi
    exit 0
fi

# ==========================================================
#  MODE: deploy
# ==========================================================
CUR_VER=$(get_current_version)
PKG_VER=$(get_package_version)

check_package_compatibility

log "============================================"
log "  AIPC Hot-swap Deploy"
log "============================================"
log "  Current version:  $CUR_VER"
log "  Package version:  $PKG_VER"
log "  Config deploy:    $([ $DEPLOY_CONFIG -eq 1 ] && echo 'yes' || echo 'skip')"
log "  Install prefix:   $INSTALL_PREFIX"
log "============================================"

if [[ $FORCE -eq 0 ]]; then
    read -rp "Proceed with deployment? [y/N] " confirm
    [[ "$confirm" =~ ^[Yy]$ ]] || { log "Aborted."; exit 0; }
fi

# Nothing below this point may stop a service until the complete package and
# the sensor-specific MediaLibrary profile have passed preflight.
write_ota_status "deploying" 50 "Package preflight"
validate_package_preflight

# --- 1. Create runtime directories ---
log ""
log "[1/8] Creating runtime directories..."
for d in "/run/aipc" "/run/aipc/shm" "/run/aipc/sockets"; do
    mkdir -p "$d"
done
mkdir -p "$INSTALL_BIN" "$INSTALL_LIB" "$INSTALL_ETC" "$INSTALL_WEB" "$INSTALL_MODELS" "$INSTALL_LOG" "$INSTALL_NGINX" "$BACKUP_BASE"
mkdir -p "$INSTALL_RECOVERY"
mkdir -p /usr/libexec

# --- 2. Backup current installation ---
log ""
log "[2/8] Backing up current installation..."
if [[ "$CUR_VER" != "unknown" ]] || ls "$INSTALL_BIN"/camera-daemon "$INSTALL_BIN"/ai-runtime &>/dev/null 2>&1; then
    create_backup
else
    log "  No existing installation found, skipping backup."
fi

# --- 3. Stop services ---
log ""
log "[3/8] Stopping services for hot-swap..."
stop_services
SERVICES_STOPPED=1

# Mutable databases, models and application state must survive rootfs
# replacement. Migrate legacy /opt paths after services stop, then retain
# compatibility through symlinks.
migrate_persistent_data

# Construct and verify the complete release while the current root still
# exists, then switch the canonical /data/aipc directory with same-filesystem
# renames. All following integration work is protected by the EXIT rollback.
prepare_staging
validate_staging
atomic_release_swap
validate_release_integrity "$INSTALL_PREFIX" "$INSTALL_PREFIX/systemd" "active release after swap"
sync

# --- 4. Activate external integration points ---
log ""
log "[4/8] Activating staged release links..."
refresh_runtime_links
if [[ -d "$INSTALL_PREFIX/libexec" ]]; then
    for f in "$INSTALL_PREFIX"/libexec/*; do
        [[ -f "$f" ]] || continue
        name=$(basename "$f")
        if is_data_backed_libexec_tool "$name"; then
            ln -sfn "$f" "/usr/libexec/$name"
        else
            install_libexec_file "$f"
        fi
    done
fi

# Fallback: deploy libexec helpers from source scripts/ when running from a
# source checkout (no pack-release staging). The exec-pre guards in systemd
# units handle the missing-helper case gracefully (skip if not installed).

_install_libexec_helper() {
    local src="$SCRIPT_DIR/$1" dest="/usr/libexec/$2"
    if os_owns_boot_control && is_os_boot_helper "$2"; then
        return 0
    fi
    if [[ -f "$src" && ! -x "$dest" ]]; then
        copy_file_no_self "$src" "$dest" 0755
        log "  + $2 -> /usr/libexec/ (from source scripts/)"
    fi
}
_install_libexec_helper aipc-compat-check.sh       aipc-compat-check
_install_libexec_helper aipc-restore.sh            aipc-restore
_install_libexec_helper aipc-firstboot-os.sh       aipc-firstboot
_install_libexec_helper aipc-autostart.sh           aipc-autostart
_install_libexec_helper aipc-osd-apply.sh           aipc-osd-apply

# Seed the persistent OSD logo config consumed by aipc-osd-apply. Lives on /data
# (survives reflash); written only when absent so later user edits are preserved.
if [[ ! -f "$INSTALL_ETC/osd-logo.conf" ]]; then
    mkdir -p "$INSTALL_ETC"
    printf 'Camthink\n' >"$INSTALL_ETC/osd-logo.conf"
    log "  + osd-logo.conf -> $INSTALL_ETC/ (default: Camthink)"
fi

# --- 5.5. Deploy model files ---
log ""
MODELS_DIR="$INSTALL_MODELS"
if [[ -d "$SCRIPT_DIR/opt/aipc/models" ]]; then
    log "  Deploying model files..."
    mkdir -p "$MODELS_DIR"
    for catdir in "$SCRIPT_DIR"/opt/aipc/models/*/; do
        [[ -d "$catdir" ]] || continue
        cat_name=$(basename "$catdir")
        mkdir -p "$MODELS_DIR/$cat_name"
        for hef in "$catdir"*.hef; do
            [[ -f "$hef" ]] || continue
            hef_name=$(basename "$hef")
            target="$MODELS_DIR/$cat_name/$hef_name"
            if [[ -f "$target" ]] && cmp -s "$hef" "$target"; then
                log "  = models/$cat_name/$hef_name (unchanged)"
            else
                cp -f "$hef" "$target"
                log "  + models/$cat_name/$hef_name -> $MODELS_DIR/$cat_name/"
            fi
        done
    done
else
    log "  No model files in package, preserving existing models."
fi

# --- 5.6. Preload factory apps ---
log ""
PRELOAD_DIR="$SCRIPT_DIR/opt/aipc/preload/apps"
if [[ -d "$PRELOAD_DIR" ]]; then
    log "[6.5/8] Preloading factory apps..."

    # Ensure containerd is running before importing images
    if ! systemctl is-active --quiet containerd; then
        systemctl start containerd
        sleep 2
    fi

    for app_dir in "$PRELOAD_DIR"/*/; do
        [[ -d "$app_dir" ]] || continue
        app_id=$(basename "$app_dir")
        manifest="$app_dir/manifest.yaml"
        image_tar="$app_dir/image.tar"
        autostart_file="$app_dir/autostart"

        # Import container image into containerd namespace
        if [[ -f "$image_tar" ]]; then
            log "  Importing image for $app_id..."
            if ctr -n aipc images import "$image_tar" >/dev/null 2>&1; then
                log "  + $app_id image imported"
            else
                warn "  $app_id image import failed (may already exist)"
            fi
        fi

        # Install manifest to app-manager manifests directory
        manifest_target=""
        if [[ -f "$manifest" ]]; then
            APPS_ETC="/etc/aipc/apps"
            mkdir -p "$APPS_ETC"
            manifest_target="$APPS_ETC/$app_id.yaml"
            if [[ ! -f "$manifest_target" ]]; then
                cp -f "$manifest" "$manifest_target"
                log "  + $app_id manifest -> $manifest_target"
            else
                log "  = $app_id manifest (already installed)"
            fi
        fi
    done

    # Register apps via platform-api InstallApp (after services start)
    # This step runs after service deployment (step 6), so we defer it.
    PRELOAD_REGISTER_DIR="/run/aipc/preload_pending"
    mkdir -p "$PRELOAD_REGISTER_DIR"
    for app_dir in "$PRELOAD_DIR"/*/; do
        [[ -d "$app_dir" ]] || continue
        app_id=$(basename "$app_dir")
        manifest="$app_dir/manifest.yaml"
        if [[ -f "$manifest" ]]; then
            cp -f "$manifest" "$PRELOAD_REGISTER_DIR/$app_id.yaml"
        fi
        # Store autostart flag
        if [[ -f "$app_dir/autostart" ]]; then
            cp -f "$app_dir/autostart" "$PRELOAD_REGISTER_DIR/$app_id.autostart"
        fi
    done
    log "  App manifests staged for registration after service start"
else
    log "  No factory apps to preload."
fi

# --- 7. Deploy configs + systemd ---
log ""
log "[7/8] Deploying configs and systemd units..."
log "  Runtime configs already installed by the validated release switch."

if [[ -d "$INSTALL_PREFIX/systemd" ]]; then
    for f in "$INSTALL_PREFIX"/systemd/*.service "$INSTALL_PREFIX"/systemd/*.timer "$INSTALL_PREFIX"/systemd/*.target; do
        [[ -f "$f" ]] || continue
        install_systemd_file "$f"
    done
fi

install_nginx_gateway_from_release
remove_obsolete_nginx_units
systemctl daemon-reload

# aipc-mcu-prep is enable-only (never start mid-deploy): it must run in the
# boot window before aipc-autostart starts the runtime daemons that own
# /dev/ttyS0. Its ConditionPathExists makes absence a clean skip.
for boot_unit in aipc-restore.service aipc-firstboot.service aipc-mcu-prep.service aipc-autostart.service aipc-os-verify.service; do
    if unit_exists "$boot_unit"; then
        if os_owns_boot_control; then
            systemctl reenable "$boot_unit" 2>/dev/null || \
                warn "Failed to re-enable OS unit $boot_unit"
        else
            systemctl enable "$boot_unit" 2>/dev/null || \
            warn "Failed to enable $boot_unit"
        fi
    fi
done

if [[ -d "$SCRIPT_DIR/opt/aipc/etc/systemd" ]]; then
    log "  + systemd config drop-ins:"
    if [[ -d "$SCRIPT_DIR/opt/aipc/etc/systemd/system.conf.d" ]]; then
        mkdir -p /etc/systemd/system.conf.d
        for f in "$SCRIPT_DIR"/opt/aipc/etc/systemd/system.conf.d/*.conf; do
            [[ -f "$f" ]] || continue
            cp -f "$f" /etc/systemd/system.conf.d/
            log "    $(basename "$f") -> /etc/systemd/system.conf.d/"
        done
    fi
    if [[ -d "$SCRIPT_DIR/opt/aipc/etc/systemd/journald.conf.d" ]]; then
        mkdir -p /etc/systemd/journald.conf.d
        for f in "$SCRIPT_DIR"/opt/aipc/etc/systemd/journald.conf.d/*.conf; do
            [[ -f "$f" ]] || continue
            cp -f "$f" /etc/systemd/journald.conf.d/
            log "    $(basename "$f") -> /etc/systemd/journald.conf.d/"
        done
    fi
fi

# Deploy sysctl drop-ins (kernel watchdog / panic self-heal). Applied at boot
# by systemd-sysctl.service and re-applied by aipc-firstboot.sh; applied now
# too because deploy is a hot-swap (no reboot).
if [[ -d "$SCRIPT_DIR/opt/aipc/etc/sysctl.d" ]]; then
    log "  + sysctl drop-ins:"
    mkdir -p /etc/sysctl.d
    for f in "$SCRIPT_DIR"/opt/aipc/etc/sysctl.d/*.conf; do
        [[ -f "$f" ]] || continue
        cp -f "$f" /etc/sysctl.d/
        log "    $(basename "$f") -> /etc/sysctl.d/"
    done
    # Apply immediately (hot deploy)
    if [[ -f /etc/sysctl.d/99-aipc-watchdog.conf ]]; then
        if sysctl -p /etc/sysctl.d/99-aipc-watchdog.conf >/dev/null 2>&1; then
            log "  Kernel self-heal sysctl applied"
        else
            warn "  sysctl -p failed (some keys may be unsupported on this kernel)"
        fi
    fi
    # Neutralize the BSP /etc/sysctl.conf core_pattern override: it loads LAST
    # in sysctl --system order (after /etc/sysctl.d/*.conf) and would override
    # our /data/core drop-in on the next boot, sending crash cores back to the
    # small root partition. Same logic as aipc-firstboot.sh step 6b.1.
    # Idempotent (grep matches only an ACTIVE line) + one-time backup via cp -n.
    if [[ -f /etc/sysctl.conf ]] && grep -qE '^[[:space:]]*kernel\.core_pattern[[:space:]]*=' /etc/sysctl.conf; then
        cp -n /etc/sysctl.conf /etc/sysctl.conf.bak.aipc 2>/dev/null || true
        sed -i -E 's|^([[:space:]]*kernel\.core_pattern[[:space:]]*=.*)|#\1  # neutralized by aipc-deploy -> /etc/sysctl.d/99-aipc-watchdog.conf|' /etc/sysctl.conf
        log "    Neutralized BSP /etc/sysctl.conf core_pattern override (-> /data/core)"
    fi
fi

# Hot-apply the systemd Manager drop-ins (log level, sp805 hardware watchdog)
# and journald drop-ins installed above, so a hot deploy (no reboot) actually
# takes effect. daemon-reexec re-reads system.conf (RuntimeWatchdogSec /
# LogLevel otherwise need a reboot); fall back to daemon-reload if reexec is
# unavailable. journald applies Storage/quota changes on SIGHUP. All guarded:
# a reload failure does NOT fail the deploy (firstboot reapplies on reboot).
systemctl daemon-reexec 2>/dev/null || systemctl daemon-reload 2>/dev/null || true
systemctl reload systemd-journald 2>/dev/null \
    || kill -HUP "$(pidof systemd-journald 2>/dev/null)" 2>/dev/null || true


# Deploy seccomp profile to system location (required by app-manager)
if [[ -f "$SCRIPT_DIR/opt/aipc/etc/security/seccomp-default.json" ]]; then
    mkdir -p /etc/aipc
    cp -f "$SCRIPT_DIR/opt/aipc/etc/security/seccomp-default.json" /etc/aipc/seccomp-default.json
    log "  + seccomp-default.json -> /etc/aipc/"
elif [[ -f "$SCRIPT_DIR/etc/aipc/seccomp-default.json" ]]; then
    mkdir -p /etc/aipc
    cp -f "$SCRIPT_DIR/etc/aipc/seccomp-default.json" /etc/aipc/seccomp-default.json
    log "  + seccomp-default.json -> /etc/aipc/"
fi

# Deploy Hailo15 imaging configs (MediaLibrary profiles, sensor calibration, etc.)
IMAGING_SOURCE="$SCRIPT_DIR/opt/aipc/etc/imaging"
if [[ -d "$IMAGING_SOURCE" ]]; then
    mkdir -p /etc/imaging
    # Only deploy if /etc/imaging doesn't already exist with custom configs
    if [[ ! -f /etc/imaging/medialib_config.json ]] && [[ ! -f /etc/imaging/medialib_configs/webserver_medialib_config.json ]]; then
        cp -rf "$IMAGING_SOURCE"/* /etc/imaging/
        log "  + imaging configs -> /etc/imaging/ (new deploy)"
    else
        log "  = imaging configs (preserved existing)"
    fi
fi

# camera-daemon.yaml (including the preflight-resolved sensor profile), web,
# Swagger, scripts and HAL are immutable staged-release content. Do not copy
# them again after the root switch; doing so would reintroduce mixed versions.

# Hot-apply fleet IMU calibration. aipc-firstboot also installs this on boot,
# but a web-triggered hot upgrade may not re-run an already-active oneshot.
CALIB_SRC="$INSTALL_PREFIX/share/calibration/final_calibration.json"
CALIB_DST="/home/root/apps/resources/final_calibration.json"
if [[ -f "$CALIB_SRC" ]]; then
    mkdir -p "$(dirname "$CALIB_DST")" 2>/dev/null || true
    if cp -f "$CALIB_SRC" "$CALIB_DST" 2>/dev/null; then
        log "  + IMU calibration -> $CALIB_DST"
    else
        warn "  Failed to install IMU calibration ($CALIB_SRC -> $CALIB_DST)"
    fi
fi

# --- 7.3. Configure containerd data directory ---
# Move containerd data to persistent data root to avoid filling rootfs.
log ""
CONTAINERD_NEEDS_FIX=0
CONTAINERD_ROOT="${AIPC_DATA_ROOT}/containerd"
CURRENT_CR=""
VARLIB_CR=""
if [[ -f /etc/containerd/config.toml ]]; then
    CURRENT_CR=$(grep '^root = ' /etc/containerd/config.toml 2>/dev/null | head -1 | sed 's/root = "//;s/"//')
    if [[ "$CURRENT_CR" != "$CONTAINERD_ROOT" ]]; then
        CONTAINERD_NEEDS_FIX=1
    fi
else
    CONTAINERD_NEEDS_FIX=1
fi
VARLIB_CR="$(readlink -f /var/lib/containerd 2>/dev/null || true)"
if [[ "$VARLIB_CR" != "$CONTAINERD_ROOT" ]]; then
    CONTAINERD_NEEDS_FIX=1
fi

if [[ $CONTAINERD_NEEDS_FIX -eq 1 ]]; then
    log "  Configuring containerd root -> $CONTAINERD_ROOT"
    systemctl stop app-manager containerd 2>/dev/null || true
    sleep 1

    # Generate default config if missing
    mkdir -p /etc/containerd
    if [[ ! -f /etc/containerd/config.toml ]] || ! grep -q '^root = ' /etc/containerd/config.toml; then
        containerd config default > /etc/containerd/config.toml
    fi

    # Migrate existing data from the configured root (or the default root), then
    # leave a compatibility symlink so stale hard-coded /var/lib/containerd
    # references do not repopulate rootfs.
    CONTAINERD_OLD_ROOT="${CURRENT_CR:-/var/lib/containerd}"
    if [[ "$CONTAINERD_OLD_ROOT" != "$CONTAINERD_ROOT" ]]; then
        migrate_persistent_dir "$CONTAINERD_OLD_ROOT" "$CONTAINERD_ROOT"
    elif [[ ! -L /var/lib/containerd && "/var/lib/containerd" != "$CONTAINERD_ROOT" ]]; then
        migrate_persistent_dir /var/lib/containerd "$CONTAINERD_ROOT"
    fi

    # Update config
    mkdir -p "$CONTAINERD_ROOT"
    sed -i "s|^root = \".*\"|root = \"${CONTAINERD_ROOT}\"|" /etc/containerd/config.toml

    systemctl start containerd
    sleep 2
    if systemctl is-active --quiet containerd; then
        log "  containerd running with root=$CONTAINERD_ROOT"
    else
        warn "  containerd failed to start after config change"
    fi
fi

# Compatibility metadata and VERSION are already part of the staged root.
# Initialize persistent schema state once, using the now-active manifest.
if [[ -f "$APP_MANIFEST_FILE" ]]; then
    mkdir -p "$(dirname "$DATA_SCHEMA_FILE")"
    if [[ ! -f "$DATA_SCHEMA_FILE" ]]; then
        schema=$(grep -o '"target_data_schema"[[:space:]]*:[[:space:]]*[0-9]*' "$APP_MANIFEST_FILE" |
            grep -o '[0-9]*' | head -1)
        [[ -n "$schema" ]] || schema=1
        printf '%s\n' "$schema" > "$DATA_SCHEMA_FILE"
    fi
    log "  + App compatibility manifest (data schema $(cat "$DATA_SCHEMA_FILE"))"
fi

# --- 8. Start services + health check ---
log ""
log "[8/8] Starting services..."
start_services
log ""
if health_check; then
    # --- 8.1 Register preloaded apps via platform-api ---
    PRELOAD_REGISTER_DIR="/run/aipc/preload_pending"
    if [[ -d "$PRELOAD_REGISTER_DIR" ]] && ls "$PRELOAD_REGISTER_DIR"/*.yaml >/dev/null 2>&1; then
        log ""
        log "Registering preloaded apps..."
        for mf in "$PRELOAD_REGISTER_DIR"/*.yaml; do
            [[ -f "$mf" ]] || continue
            app_id=$(basename "$mf" .yaml)
            manifest_path="/etc/aipc/apps/$app_id.yaml"

            # Call InstallApp API — image already imported via ctr
            result=$(curl -sf -X POST http://localhost:8080/api/v1/apps \
                -H "Content-Type: application/json" \
                -H "Authorization: Bearer "${AIPC_TOKEN_KEY:-}"" \
                -d "{\"manifest_path\": \"$manifest_path\"}" 2>/dev/null)
            if [[ -n "$result" ]]; then
                log "  + $app_id registered"
            else
                log "  = $app_id (already registered or install skipped)"
            fi

            # Check autostart flag
            if [[ -f "$PRELOAD_REGISTER_DIR/$app_id.autostart" ]]; then
                autostart=$(cat "$PRELOAD_REGISTER_DIR/$app_id.autostart" 2>/dev/null)
                if [[ "$autostart" == "true" ]]; then
                    curl -sf -X POST "http://localhost:8080/api/v1/apps/$app_id/start" \
                        -H "Authorization: Bearer "${AIPC_TOKEN_KEY:-}"" >/dev/null 2>&1
                    log "  > $app_id autostarted"
                fi
            fi
        done
        # Cleanup pending dir
        rm -rf "$PRELOAD_REGISTER_DIR"
    fi

    DEPLOY_COMMITTED=1
    SERVICES_STOPPED=0
    SWAP_ACTIVE=0
    log "============================================"
    log "  Deploy successful!"
    log "  Version: $PKG_VER"
    log "============================================"
    log ""
    log "  View logs:     journalctl -u 'camera-daemon' -f"
    log "  Rollback:      ./deploy.sh --rollback"
    log "  Check status:  ./deploy.sh --status"
    log "============================================"
else
    FAILURE_DETAIL="line $LINENO, exit=1: health_check"
    err "============================================"
    err "  Deploy completed but health check FAILED"
    err "============================================"
    err ""
    err "  Some services did not start properly."
    err "  Check logs:  journalctl -u '<service>' -n 50"
    err ""
    err "  To rollback: ./deploy.sh --rollback --force"
    err "============================================"
    exit 1
fi
