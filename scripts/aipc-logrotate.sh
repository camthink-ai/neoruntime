#!/bin/bash
# aipc-logrotate.sh — size-based rotation for Go service logs that lack built-in
# rotation. platform/common/logger opens logs with O_APPEND and NEVER rotates, so
# without this an INFO-level service (e.g. event-bus, ~600 MiB/day) fills the
# partition. The device image ships NO logrotate and NO cron, so this runs under
# a systemd timer (aipc-logrotate.timer, every 10 min).
#
# camera-daemon (C++) rotates its own logs (.1-.5) and is intentionally EXCLUDED
# to avoid suffix collisions; only the unbounded Go service logs are rotated.
#
# Rotation per log (when size > MAX_BYTES):
#   drop oldest .5(+.gz); shift .4->.5, .3->.4, .2->.3, .1->.2;
#   copy current -> .1; gzip .1; truncate current IN PLACE.
# Truncate-in-place keeps the same inode, so the Go process's O_APPEND fd stays
# valid and writing without a restart — equivalent to logrotate "copytruncate".
# (A few lines written between the copy and the truncate can be lost; this is
# the standard copytruncate trade-off and acceptable for logs.)

set -u

# Prevent concurrent runs (timer + manual, or two timer fires) from racing on
# the same log files — simultaneous rotations produce duplicate snapshots.
# Non-blocking: if another run holds the lock, exit cleanly.
exec 9>/run/aipc-logrotate.lock
flock -n 9 || exit 0

MAX_BYTES=$((10 * 1024 * 1024))   # 10 MiB threshold per file
KEEP=5                            # rotated generations retained (.1 .. .5)
GZIP_ROTATED=1                    # 1 = gzip .1 after snapshotting

# Go service logs known to have no built-in rotation. Add new Go services here
# (or, better long-term, switch logger.go to lumberjack — tracked as P1).
LOG_NAMES=(
    event-bus
    device-control
    platform-api
    app-manager
    ai-runtime
    # Non-Go Hailo media-pipeline logs (VC8000 video encoder + media lib) — these
    # are also unbounded and live on /data/logs. vc8000e.log alone reaches
    # hundreds of MiB over days. Same copytruncate mechanism; the writer reopens
    # / holds O_APPEND so truncating in place keeps its fd valid.
    vc8000e
    medialib
)
# Log directories to rotate. Order doesn't matter; non-existent dirs are skipped
# below, so listing extra candidates is harmless.
#   /data/logs      — persistent primary. logger.go hardcodes the preference for
#                     non-SetRootPath services (event-bus/device-control/ai-runtime),
#                     and SetRootPath services running under the default prefix
#                     /data land here too. Always covered.
#   /data/aipc/logs — common persistent install prefix used by field devices.
#                     Include it unconditionally so timer units that do not
#                     export AIPC_PREFIX still cover VC8000/media logs there.
#   /opt/aipc/logs  — legacy default-prefix writer (root partition); bounded until
#                     it migrates to /data.
# If a custom install prefix is ever used (deploy.sh --prefix /data/aipc), the
# SetRootPath services (platform-api, app-manager) move their logs to $PREFIX/logs
# while the rest stay on /data/logs. Export AIPC_PREFIX (or INSTALL_PREFIX) so the
# prefixed dir is also covered — e.g. Environment=AIPC_PREFIX=/data/aipc in the
# timer's service unit. With the default prefix this block is a no-op.
LOG_DIRS=(/data/logs /data/aipc/logs /opt/aipc/logs)
for _p in "${AIPC_PREFIX:-}" "${INSTALL_PREFIX:-}"; do
    [[ -n "$_p" && " ${LOG_DIRS[*]} " != *" ${_p}/logs "* ]] && LOG_DIRS+=("$_p/logs")
done

rotate_one() {
    local f="$1"
    [[ -f "$f" ]] || return 0
    local sz
    sz=$(stat -c%s "$f" 2>/dev/null) || return 0
    [[ "$sz" -gt "$MAX_BYTES" ]] || return 0

    # Drop the oldest generation, then shift the rest up (high -> low to avoid
    # clobbering).
    rm -f "${f}.${KEEP}" "${f}.${KEEP}.gz" 2>/dev/null
    local i
    for ((i = KEEP - 1; i >= 1; i--)); do
        [[ -f "${f}.${i}" ]]    && mv -f "${f}.${i}"    "${f}.$((i + 1))"    2>/dev/null
        [[ -f "${f}.${i}.gz" ]] && mv -f "${f}.${i}.gz" "${f}.$((i + 1)).gz" 2>/dev/null
    done

    # Snapshot current -> .1, then truncate current in place (keeps inode/fd).
    cp -p "$f" "${f}.1" 2>/dev/null
    : > "$f"
    [[ "$GZIP_ROTATED" = "1" ]] && gzip -f "${f}.1" 2>/dev/null
}

for dir in "${LOG_DIRS[@]}"; do
    [[ -d "$dir" ]] || continue
    for name in "${LOG_NAMES[@]}"; do
        rotate_one "${dir}/${name}.log"
    done
done

exit 0
