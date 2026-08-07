#!/bin/bash
# ============================================================================
# AIPC Platform — Health Monitor (black-box sampler)
# ============================================================================
# Purpose: capture the pre-hang state of the device on PERSISTENT storage so
# that when the box freezes hard (SSH/network dead — the 93.72 failure mode),
# the last samples before the freeze are readable after the reboot.
#
# Design constraints:
#   - The device is unreachable when it hangs, so we CANNOT rely on network
#     upload. Everything is written locally to /data/health (persistent).
#   - Sampling must be cheap (< 1s) and never aggravate the system under
#     stress, so we read /proc and /sys directly and avoid heavy tools.
#   - Failure-tolerant: a missing file or unsupported tool must NOT abort the
#     loop — `set -e` is intentionally omitted.
#
# Output: /data/health/snapshots.log (ring buffer, tail-truncated at
#         HEALTHMON_MAX_BYTES). One block per sample, led by a UTC ISO-8601
#         timestamp so the last-written block is the pre-hang moment.
#
# Tunables (env, override in the unit file):
#   HEALTHMON_INTERVAL   seconds between samples     (default 5)
#   HEALTHMON_MAX_BYTES  ring-buffer size cap in bytes (default 5242880 = 5 MiB)
#   HEALTHMON_LOG_DIR    output directory            (default /data/health)
#   HEALTHMON_SERVICES   space-separated services to probe
# ============================================================================

set -uo pipefail   # NOTE: no -e — a failing probe must not kill the monitor

INTERVAL="${HEALTHMON_INTERVAL:-5}"
MAX_BYTES="${HEALTHMON_MAX_BYTES:-5242880}"
LOG_DIR="${HEALTHMON_LOG_DIR:-/data/health}"
LOG_FILE="$LOG_DIR/snapshots.log"
# shellcheck disable=SC2206
SERVICES=(${HEALTHMON_SERVICES:-ai-runtime camera-daemon app-manager event-bus device-control})

mkdir -p "$LOG_DIR" 2>/dev/null || true

# ---------- ring-buffer rotation: keep the tail when the file grows too big --
rotate() {
    local sz
    sz=$(stat -c %s "$LOG_FILE" 2>/dev/null || echo 0)
    if [[ "$sz" -gt "$MAX_BYTES" ]]; then
        local keep=$(( MAX_BYTES * 8 / 10 ))   # retain last 80%
        tail -c "$keep" "$LOG_FILE" > "$LOG_FILE.tmp" 2>/dev/null && mv "$LOG_FILE.tmp" "$LOG_FILE" 2>/dev/null
    fi
}

# ---------- one sample -------------------------------------------------------
snapshot() {
    local ts
    ts=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
    local up
    up=$(awk '{printf "%.0fs", $1}' /proc/uptime 2>/dev/null || echo "n/a")
    local load
    load=$(cut -d' ' -f1-3 /proc/loadavg 2>/dev/null || echo "n/a")
    {
        echo "=== $ts (up=$up load=$load) ==="

        # memory (OOM precursor)
        echo "[mem] $(grep -E '^(MemTotal|MemAvailable|MemFree|Buffers|Cached|SwapTotal|SwapFree):' /proc/meminfo 2>/dev/null | tr '\n' ' ')"

        # thermal zones
        local temps=""
        local z v
        for z in /sys/class/thermal/thermal_zone*/temp; do
            [[ -r "$z" ]] || continue
            v=$(cat "$z" 2>/dev/null) || continue
            temps+="${z#/sys/class/thermal/}=$(awk -v t="$v" 'BEGIN{printf "%.1fC", t/1000}') "
        done
        echo "[thermal] ${temps:-n/a}"

        # top CPU / memory consumers (procps syntax; empty under busybox)
        echo "[top-cpu] $(ps --no-headers -eo %cpu,rss,comm --sort=-%cpu 2>/dev/null | head -5 | awk '{printf "%s%%,%dK,%s | ",$1,$2,$3}')"
        echo "[top-mem] $(ps --no-headers -eo rss,comm --sort=-rss 2>/dev/null | head -5 | awk '{printf "%dK,%s | ",$1,$2}')"

        # platform service liveness
        local svc_line="" s
        for s in "${SERVICES[@]}"; do
            svc_line+="$s=$(systemctl is-active "$s" 2>/dev/null || echo '?') "
        done
        echo "[services] $svc_line"

        # IPC sockets presence (proxy for ai-runtime/app reachability, no grpc dep)
        local sock_line="" sock
        for sock in /run/aipc/sockets/*.sock; do
            [[ -S "$sock" ]] && sock_line+="${sock##*/} "
        done
        echo "[sockets] ${sock_line:-none}"

        # disk (a full /data also wedges the device)
        echo "[disk] $(df -P / /data 2>/dev/null | awk 'NR>1{printf "%s=%s%% ", $6, $5}')"

        # network interface counters (rx/tx bytes) from /proc — lighter than `ip -s`
        echo "[net] $(awk 'NR>2 && $1 !~ /lo/{iface=$1; sub(/:/,"",iface); printf "%s_rx=%s_tx=%s ", iface, $2, $10}' /proc/net/dev 2>/dev/null)"

        # last 3 kernel messages (reltime) — needs root, which the unit has
        echo "[dmesg] $(dmesg --time-format reltime 2>/dev/null | tail -3 | tr '\n' '|')"
    } >> "$LOG_FILE" 2>/dev/null
}

# ---------- graceful shutdown -----------------------------------------------
cleanup() {
    {
        echo "=== $(date -u '+%Y-%m-%dT%H:%M:%SZ') healthmon stopping (received signal) ==="
    } >> "$LOG_FILE" 2>/dev/null || true
    exit 0
}
trap cleanup TERM INT

# ---------- wait for /data to be mounted (firstboot may run in parallel) ----
for _ in $(seq 1 20); do
    [[ -d /data ]] && break
    sleep 1
done

# ---------- main loop --------------------------------------------------------
# `sleep & wait` so SIGTERM is handled promptly instead of after a full sleep.
echo "=== $(date -u '+%Y-%m-%dT%H:%M:%SZ') healthmon started (interval=${INTERVAL}s, cap=${MAX_BYTES}B) ===" >> "$LOG_FILE" 2>/dev/null || true
while true; do
    snapshot
    rotate
    sleep "$INTERVAL" &
    wait $!
done
