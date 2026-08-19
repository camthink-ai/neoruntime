#!/bin/bash
# Enable and asynchronously start the AIPC runtime after restore/bootstrap.

set -uo pipefail

SERVICES=(
    aipc-healthmon
    event-bus
    camera-daemon
    ai-runtime
    device-control
    device-discovery
    platform-api
    app-manager
    aipc-nginx-gateway
)

log() { echo "[aipc-autostart] $*"; }

systemctl daemon-reload

units=()
for service in "${SERVICES[@]}"; do
    unit="${service}.service"
    if ! systemctl cat "$unit" >/dev/null 2>&1; then
        log "Skipping $unit (not installed)"
        continue
    fi

    state="$(systemctl is-enabled "$unit" 2>/dev/null || true)"
    case "$state" in
        masked|masked-runtime)
            log "Skipping $unit ($state)"
            continue
            ;;
    esac

    if ! systemctl enable --no-reload "$unit" >/dev/null 2>&1; then
        log "WARN: failed to enable $unit"
    fi
    units+=("$unit")
done

systemctl daemon-reload

if (( ${#units[@]} == 0 )); then
    log "No AIPC runtime services are installed"
    exit 0
fi

# Never wait for the runtime from this boot oneshot. The daemon units express
# their own dependency order and may take time to become healthy.
if ! systemctl start --no-block "${units[@]}"; then
    log "WARN: one or more AIPC services could not be queued"
    exit 1
fi

log "Queued ${#units[@]} AIPC runtime services"
