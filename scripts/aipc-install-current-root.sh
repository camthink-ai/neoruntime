#!/bin/bash
# Rebuild the rootfs-local AIPC integration from the persistent /data release.
#
# The OS image owns only the generic aipc-app-bootstrap launcher. Everything
# that can change with an AIPC release (units, helpers, binaries, compatibility
# fields and configuration drop-ins) is restored by this application-owned,
# slot-agnostic installer.

set -euo pipefail

DATA_ROOT="${AIPC_INSTALL_ROOT:-/data/aipc}"
ROOTFS_PREFIX="${AIPC_ROOTFS_PREFIX:-}"
SYSTEMCTL="${AIPC_SYSTEMCTL:-systemctl}"
SYSCTL="${AIPC_SYSCTL:-sysctl}"
LDCONFIG="${AIPC_LDCONFIG:-ldconfig}"
ACTIVATE="${AIPC_ACTIVATE:-1}"
TAG="aipc-current-root"

root_path() { printf '%s%s' "$ROOTFS_PREFIX" "$1"; }
NGINX_ROOT="$(root_path "${AIPC_NGINX_ROOT:-/data/nginx}")"
log() { echo "[$TAG] $*"; }
warn() { echo "[$TAG] WARN: $*" >&2; }
fail() { echo "[$TAG] ERROR: $*" >&2; exit 1; }

copy_file_no_self() {
    local source="$1" dest="$2" mode="${3:-}"
    if [[ -e "$dest" && "$source" -ef "$dest" ]]; then
        [[ -z "$mode" ]] || chmod "$mode" "$dest"
        return 0
    fi
    cp -f "$source" "$dest"
    [[ -z "$mode" ]] || chmod "$mode" "$dest"
}

require_nonempty() {
    local path="$1"
    [[ -f "$path" && -s "$path" ]] || fail "missing or empty release file: $path"
}

require_executable() {
    local path="$1"
    [[ -f "$path" && -s "$path" && -x "$path" ]] || fail "missing, empty, or non-executable release file: $path"
}

configure_platform_api_gateway_mode() {
    local mode="$1" helper="$DATA_ROOT/scripts/aipc-configure-platform-api-gateway.py"
    [[ -x "$helper" && -f "$DATA_ROOT/etc/platform-api.yaml" ]] || return 0
    /usr/bin/python3 "$helper" --config "$DATA_ROOT/etc/platform-api.yaml" --mode "$mode" || \
        warn "failed to configure platform-api gateway mode: $mode"
}

validate_release_root() {
    local file empty
    for file in \
        "$DATA_ROOT/app-manifest.json" \
        "$DATA_ROOT/VERSION" \
        "$DATA_ROOT/systemd/aipc-platform.target" \
        "$DATA_ROOT/etc/platform-api.yaml" \
        "$DATA_ROOT/etc/camera-daemon.yaml"; do
        require_nonempty "$file"
    done
    for file in \
        "$DATA_ROOT/bin/platform-api" \
        "$DATA_ROOT/bin/camera-daemon" \
        "$DATA_ROOT/libexec/aipc-compat-check" \
        "$DATA_ROOT/libexec/aipc-os-updater" \
        "$DATA_ROOT/scripts/aipc-configure-platform-api-gateway.py" \
        "$DATA_ROOT/scripts/aipc-install-current-root.sh" \
        "$DATA_ROOT/scripts/aipc-firstboot.sh"; do
        require_executable "$file"
    done
    while IFS= read -r empty; do
        fail "empty immutable release file: $empty"
    done < <(find "$DATA_ROOT/bin" "$DATA_ROOT/docs" "$DATA_ROOT/etc" \
        "$DATA_ROOT/firmware" "$DATA_ROOT/libexec" "$DATA_ROOT/recovery" \
        "$DATA_ROOT/nginx" "$DATA_ROOT/scripts" "$DATA_ROOT/share" \
        "$DATA_ROOT/swagger-ui" "$DATA_ROOT/systemd" "$DATA_ROOT/web" \
        -type f -size 0 -print 2>/dev/null)
}

install_nginx_gateway() {
    local source="$DATA_ROOT/nginx" file name target
    [[ -d "$source" ]] || return 0

    mkdir -p \
        "$NGINX_ROOT/bin" "$NGINX_ROOT/conf" "$NGINX_ROOT/sbin" "$NGINX_ROOT/run" "$NGINX_ROOT/logs" \
        "$NGINX_ROOT/tmp/client_body" "$NGINX_ROOT/tmp/proxy" \
        "$NGINX_ROOT/tmp/fastcgi" "$NGINX_ROOT/tmp/uwsgi" "$NGINX_ROOT/tmp/scgi"

    if [[ -d "$source/runtime" ]]; then
        if [[ -d "$source/runtime/bin" ]]; then
            mkdir -p "$NGINX_ROOT/bin"
            cp -aP "$source/runtime/bin"/. "$NGINX_ROOT/bin"/
            chmod 0755 "$NGINX_ROOT/bin/nginx" 2>/dev/null || true
        fi
        if [[ -d "$source/runtime/rootfs" ]]; then
            rm -rf "$NGINX_ROOT/rootfs"
            mkdir -p "$NGINX_ROOT/rootfs"
            cp -aP "$source/runtime/rootfs"/. "$NGINX_ROOT/rootfs"/
            chmod 0755 "$NGINX_ROOT/rootfs/usr/sbin/nginx" 2>/dev/null || true
        fi
    fi

    if [[ -d "$source/conf" ]]; then
        for file in "$source"/conf/*; do
            [[ -f "$file" ]] || continue
            name="$(basename -- "$file")"
            target="$NGINX_ROOT/conf/$name"
            if [[ "$name" == *.seed ]]; then
                target="$NGINX_ROOT/conf/${name%.seed}"
                [[ -s "$target" ]] && continue
            fi
            copy_file_no_self "$file" "$target" 0644
        done
    fi

    if [[ -d "$source/sbin" ]]; then
        for file in "$source"/sbin/*; do
            [[ -f "$file" ]] || continue
            copy_file_no_self "$file" "$NGINX_ROOT/sbin/$(basename -- "$file")" 0755
        done
    fi

    if [[ -x "$NGINX_ROOT/bin/nginx" ]]; then
        configure_platform_api_gateway_mode nginx
        if [[ -x "$NGINX_ROOT/sbin/aipc-nginx-app-route-sync.py" ]]; then
            /usr/bin/python3 "$NGINX_ROOT/sbin/aipc-nginx-app-route-sync.py" --ensure-cert >/dev/null 2>&1 || \
                warn "initial nginx app route generation failed; the gateway service will retry"
        fi
    fi
}

MANIFEST="$DATA_ROOT/app-manifest.json"
UNIT_SOURCE="$DATA_ROOT/systemd"
OS_RELEASE_FILE="$(root_path /etc/aipc-os-release)"
COMPAT_CHECK="$DATA_ROOT/libexec/aipc-compat-check"

[[ -f "$MANIFEST" ]] || fail "missing persistent app manifest: $MANIFEST"
[[ -d "$UNIT_SOURCE" ]] || fail "missing canonical systemd units: $UNIT_SOURCE"
[[ -f "$OS_RELEASE_FILE" ]] || fail "OS compatibility stub is missing: $OS_RELEASE_FILE"
[[ -x "$COMPAT_CHECK" ]] || fail "missing compatibility checker: $COMPAT_CHECK"
validate_release_root

json_number() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$1" | head -1
}

json_string() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1
}

# Compatibility capabilities are OS-owned. The application may validate them
# but must never rewrite them from its own requirements.
grep -Eq '^MACHINE=.+$' "$OS_RELEASE_FILE" || fail "OS compatibility metadata has no MACHINE"
grep -Eq '^AIPC_COMPAT_LEVEL=[1-9][0-9]*$' "$OS_RELEASE_FILE" || \
    fail "OS compatibility metadata has invalid AIPC_COMPAT_LEVEL"
grep -Eq '^DATA_SCHEMA=[1-9][0-9]*$' "$OS_RELEASE_FILE" || \
    fail "OS compatibility metadata has invalid DATA_SCHEMA"
[[ -n "$(json_string "$MANIFEST" machine)" && -n "$(json_number "$MANIFEST" required_compat_level)" && \
   -n "$(json_number "$MANIFEST" target_data_schema)" ]] || \
    fail "persistent app manifest has incomplete compatibility metadata"

if [[ -z "$ROOTFS_PREFIX" ]]; then
    AIPC_OS_COMPATIBILITY_FILE="$OS_RELEASE_FILE" \
    AIPC_APP_MANIFEST="$MANIFEST" \
    "$COMPAT_CHECK"
fi

# Recreate rootfs-local executable links. The real files live on /data and
# survive both A/B slot switches and single-copy rootfs replacement.
USR_BIN_DIR="$(root_path /usr/bin)"
USR_LIBEXEC_DIR="$(root_path /usr/libexec)"
mkdir -p "$USR_BIN_DIR" "$USR_LIBEXEC_DIR"
for file in "$DATA_ROOT"/bin/*; do
    [[ -f "$file" && -x "$file" ]] || continue
    name="$(basename -- "$file")"
    case "$name" in *.py) continue ;; esac
    ln -sfn "$file" "$USR_BIN_DIR/$name"
done
for file in "$DATA_ROOT"/libexec/*; do
    [[ -f "$file" && -x "$file" ]] || continue
    name="$(basename -- "$file")"
    ln -sfn "$file" "$USR_LIBEXEC_DIR/$name"
done

# Restore application-owned rootfs configuration removed by an OS rewrite.
SYSTEM_CONF_DIR="$(root_path /etc/systemd/system.conf.d)"
JOURNAL_CONF_DIR="$(root_path /etc/systemd/journald.conf.d)"
SYSCTL_DIR="$(root_path /etc/sysctl.d)"
SECURITY_DIR="$(root_path /etc/aipc)"
LDCONFIG_DIR="$(root_path /etc/ld.so.conf.d)"
mkdir -p "$SYSTEM_CONF_DIR" "$JOURNAL_CONF_DIR" "$SYSCTL_DIR" "$SECURITY_DIR" "$LDCONFIG_DIR"

for file in "$DATA_ROOT"/etc/systemd/system.conf.d/*.conf; do
    [[ -f "$file" ]] && copy_file_no_self "$file" "$SYSTEM_CONF_DIR/$(basename -- "$file")" 0644
done
for file in "$DATA_ROOT"/etc/systemd/journald.conf.d/*.conf; do
    [[ -f "$file" ]] && copy_file_no_self "$file" "$JOURNAL_CONF_DIR/$(basename -- "$file")" 0644
done
for file in "$DATA_ROOT"/etc/sysctl.d/*.conf; do
    [[ -f "$file" ]] || continue
    copy_file_no_self "$file" "$SYSCTL_DIR/$(basename -- "$file")" 0644
    if [[ -z "$ROOTFS_PREFIX" && "$ACTIVATE" == "1" ]]; then
        "$SYSCTL" -p "$SYSCTL_DIR/$(basename -- "$file")" >/dev/null 2>&1 || \
            warn "failed to apply $(basename -- "$file")"
    fi
done
if [[ -f "$DATA_ROOT/etc/security/seccomp-default.json" ]]; then
    copy_file_no_self "$DATA_ROOT/etc/security/seccomp-default.json" "$SECURITY_DIR/seccomp-default.json" 0644
fi
printf '%s\n' "$DATA_ROOT/lib/hal" >"$LDCONFIG_DIR/aipc.conf"
install_nginx_gateway

# Promote and enable the exact unit set staged by this AIPC release. The source
# directory is mirrored during deploy, so removed services do not survive as
# stale units in a later release.
SYSTEMD_DIR="$(root_path /etc/systemd/system)"
mkdir -p "$SYSTEMD_DIR"
for legacy in nginx-data.service aipc-nginx-app-routes.service; do
    if [[ -z "$ROOTFS_PREFIX" && "$ACTIVATE" == "1" ]]; then
        "$SYSTEMCTL" disable --now "$legacy" >/dev/null 2>&1 || true
    fi
    rm -f "$SYSTEMD_DIR/$legacy"
done
shopt -s nullglob
units=("$UNIT_SOURCE"/*.service "$UNIT_SOURCE"/*.timer "$UNIT_SOURCE"/*.target)
(( ${#units[@]} > 0 )) || fail "canonical systemd unit set is empty"

for unit in "${units[@]}"; do
    name="$(basename -- "$unit")"
    copy_file_no_self "$unit" "$SYSTEMD_DIR/$name" 0644
    if [[ -z "$ROOTFS_PREFIX" && "$ACTIVATE" == "1" ]]; then
        "$SYSTEMCTL" enable "$name" >/dev/null 2>&1 || warn "could not enable $name"
    fi
done

[[ -f "$SYSTEMD_DIR/aipc-platform.target" ]] || fail "aipc-platform.target is missing from canonical units"

if [[ -z "$ROOTFS_PREFIX" && "$ACTIVATE" == "1" ]]; then
    "$LDCONFIG" 2>/dev/null || warn "ldconfig failed"
    "$SYSTEMCTL" daemon-reexec 2>/dev/null || "$SYSTEMCTL" daemon-reload 2>/dev/null || \
        fail "systemd reload failed"
    "$SYSTEMCTL" try-restart systemd-journald.service >/dev/null 2>&1 || true
    # Start only the application boot chain and stable platform target. Starting
    # every copied oneshot would incorrectly trigger maintenance-only services
    # such as aipc-os-updater or aipc-os-reboot.
    boot_units=(
        aipc-restore.service
        aipc-firstboot.service
        aipc-mcu-prep.service
        aipc-autostart.service
        aipc-platform.target
        aipc-os-verify.service
        aipc-logrotate.timer
    )
    "$SYSTEMCTL" start --no-block "${boot_units[@]}" >/dev/null 2>&1 || \
        fail "failed to queue promoted AIPC units"
fi

log "installed ${#units[@]} unit(s) and rebuilt current-root integration"
