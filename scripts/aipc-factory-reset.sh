#!/bin/bash
# Factory reset: restore the device to default configuration.
#
# Production-line step: run AFTER functional tests, BEFORE release. Clears
# everything an operator or commissioning may have configured and leaves the
# platform in its just-deployed state:
#
#   RESET (whitelist, nothing else is touched):
#     - /data/aipc/data/platform.db*        platform DB (apps, instances,
#                                            settings, web login) -> re-seeded
#                                            by platform-api on next start
#     - /data/aipc/apps/instances,registry  app runtime state
#     - /data/aipc/data/event-bus           persisted events
#     - /data/aipc/data/media-backup        media backup cache
#     - /data/aipc/network/*.network        commissioned per-device IP
#     - /etc/systemd/network/10-eth0.network -> image default 10.0.0.1/24
#
#   PRESERVED (never touched):
#     - /data/aipc binaries, web, models     (product content, not config)
#     - SN / MAC                             (U-Boot env + MCU EEPROM, outside
#                                             /data/aipc entirely)
#     - logs under /data/aipc/log            (kept for RMA traceability)
#
# Optional: AIPC_RESET_PAYLOAD_DIR=/path/to/release/opt/aipc also restores
# /data/aipc/etc/*.yaml from the packaged defaults.
#
# Usage:
#   aipc-factory-reset.sh [--yes] [--dry-run]
#
#   --yes       skip the interactive confirmation
#   --dry-run   print the planned actions, change nothing
#
# Environment:
#   AIPC_ROOT               install root (default /data/aipc; any other value
#                           implies TEST MODE: service/network control on the
#                           host is skipped so the script is safe to exercise
#                           on a dev box)

set -euo pipefail

AIPC_ROOT="${AIPC_ROOT:-/data/aipc}"
PLATFORM_TARGET="aipc-platform.target"
DEFAULT_NETWORK_FILE="/etc/systemd/network/10-eth0.network"
DEFAULT_NETWORK_CONF="[Match]
Name=eth0

[Network]
Address=10.0.0.1/24"

ASSUME_YES=0
DRY_RUN=0

log() { echo "[aipc-factory-reset] $*"; }

fail() {
    log "ERROR: $*"
    exit 1
}

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --yes) ASSUME_YES=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help) usage ;;
        *) fail "unknown option: $1 (see --help)" ;;
    esac
    shift
done

# ---------- guards ----------

[[ ${EUID} -eq 0 ]] || fail "must run as root"

# Refuse anything that does not look like a real install root. The VERSION
# file is deployed by deploy.sh and validated by aipc-restore.sh; it is the
# cheapest reliable marker of "this is (a copy of) /data/aipc".
[[ "$AIPC_ROOT" == /* ]] || fail "AIPC_ROOT must be an absolute path: $AIPC_ROOT"
[[ ${#AIPC_ROOT} -gt 4 ]] || fail "refusing suspiciously short AIPC_ROOT: $AIPC_ROOT"
[[ -f "$AIPC_ROOT/VERSION" ]] || fail "not an AIPC install root (no VERSION file): $AIPC_ROOT"

# Service/network control only runs against the real device root. With an
# overridden AIPC_ROOT (tests, inspection of a mounted data partition) the
# script must not stop host services or rewrite the host's network config.
IS_DEVICE=0
if [[ "$AIPC_ROOT" == "/data/aipc" ]]; then
    IS_DEVICE=1
fi

# ---------- helpers ----------

# rm_exec <path>... : remove files/dirs, honoring DRY_RUN.
rm_exec() {
    local path
    for path in "$@"; do
        if [[ -e "$path" ]]; then
            if [[ $DRY_RUN -eq 1 ]]; then
                log "DRY-RUN  rm -rf $path"
            else
                rm -rf "$path"
                log "removed  $path"
            fi
        fi
    done
}

svc() {
    # systemctl wrapper: no-op in DRY_RUN / TEST MODE.
    if [[ $DRY_RUN -eq 1 ]]; then
        log "DRY-RUN  systemctl $*"
    elif [[ $IS_DEVICE -eq 1 ]]; then
        systemctl "$@" || true
    else
        log "TEST MODE: skip systemctl $*"
    fi
}

# ---------- plan ----------

log "factory reset target : $AIPC_ROOT"
log "default IP after reset: 10.0.0.1/24 (eth0)"
log "preserved            : binaries/web/models, SN/MAC, logs"
echo

if [[ $DRY_RUN -ne 1 && $ASSUME_YES -ne 1 ]]; then
    read -r -p "This erases configured apps, settings and the commissioned IP. Continue? [y/N] " answer
    case "$answer" in
        y|Y|yes|YES) ;;
        *) log "aborted"; exit 1 ;;
    esac
fi

# ---------- reset ----------

log "=== [1/5] Stopping platform services ==="
svc stop "$PLATFORM_TARGET"

log "=== [2/5] Clearing platform state ==="
rm_exec "$AIPC_ROOT/data/platform.db" \
    "$AIPC_ROOT/data/platform.db-wal" \
    "$AIPC_ROOT/data/platform.db-shm" \
    "$AIPC_ROOT/apps/instances" \
    "$AIPC_ROOT/apps/registry" \
    "$AIPC_ROOT/data/event-bus" \
    "$AIPC_ROOT/data/media-backup"

log "=== [3/5] Restoring packaged /data/aipc/etc defaults ==="
if [[ -n "${AIPC_RESET_PAYLOAD_DIR:-}" ]]; then
    if [[ -d "$AIPC_RESET_PAYLOAD_DIR/etc" ]]; then
        if [[ $DRY_RUN -eq 1 ]]; then
            log "DRY-RUN  cp $AIPC_RESET_PAYLOAD_DIR/etc/*.yaml -> $AIPC_ROOT/etc/"
        else
            cp -f "$AIPC_RESET_PAYLOAD_DIR"/etc/*.yaml "$AIPC_ROOT/etc/" 2>/dev/null || true
            log "restored $AIPC_ROOT/etc from $AIPC_RESET_PAYLOAD_DIR/etc"
        fi
    else
        log "WARN: AIPC_RESET_PAYLOAD_DIR has no etc/ dir, skipping"
    fi
else
    log "skipped (set AIPC_RESET_PAYLOAD_DIR to also restore service yaml defaults)"
fi

log "=== [4/5] Resetting network to default (10.0.0.1/24) ==="
rm_exec "$AIPC_ROOT"/network/*.network
if [[ $DRY_RUN -eq 1 ]]; then
    log "DRY-RUN  write $DEFAULT_NETWORK_FILE (static 10.0.0.1/24)"
elif [[ $IS_DEVICE -eq 1 ]]; then
    printf '%s\n' "$DEFAULT_NETWORK_CONF" >"$DEFAULT_NETWORK_FILE"
    chmod 0644 "$DEFAULT_NETWORK_FILE"
    log "wrote $DEFAULT_NETWORK_FILE (static 10.0.0.1/24)"
    if command -v networkctl >/dev/null 2>&1; then
        networkctl reload 2>/dev/null || true
        networkctl reconfigure eth0 2>/dev/null || true
    fi
    log "WARN: if you are connected via eth0 at the commissioned IP, the link"
    log "WARN: address is about to move to 10.0.0.1 — reconnect there."
else
    log "TEST MODE: skip rewriting host $DEFAULT_NETWORK_FILE"
fi

log "=== [5/5] Starting platform services ==="
svc start "$PLATFORM_TARGET"
if [[ $IS_DEVICE -eq 1 && $DRY_RUN -ne 1 ]]; then
    log "platform-api will re-create and re-seed platform.db on startup"
fi

echo
log "factory reset complete. Device default address: 10.0.0.1/24"
