#!/bin/bash
# AIPC baseboard MCU boot prep — RTC sync (host -> MCU) + firmware OTA.
#
# Runs once at boot BEFORE aipc-autostart, i.e. before any runtime daemon
# (camera-daemon / device-control) opens /dev/ttyS0. The host UART is
# exclusive-use and owned by the runtime HAL, so MCU prep MUST finish in this
# pre-runtime window. See systemd/aipc-mcu-prep.service for the ordering.
#
# This wrapper NEVER blocks boot:
#   - missing binary / firmware package -> skip that phase, log, continue
#   - any ne503_boot_prep non-zero exit  -> map to a human log line, continue
#   - always exits 0
#
# ne503_boot_prep exit codes (see /home/share/USAGE.md):
#   0 success / skip, 1 param error, 2 HAL_MCU_OPS.init fail (MCU not
#   responding or UART busy), 3 RTC step fail, 4 firmware header check fail,
#   5 OTA failed after 3 retries.

set -uo pipefail

BIN="${AIPC_MCU_PREP_BIN:-/data/aipc/bin/ne503_boot_prep}"
FW_DIR="${AIPC_MCU_FW_DIR:-/data/aipc/firmware/mcu}"
RUN_DIR="${AIPC_MCU_PREP_RUN_DIR:-/run/aipc/mcu-prep}"
ONCE_MARKER="${AIPC_MCU_PREP_ONCE_MARKER:-$RUN_DIR/attempted}"
BOOT_PREP_TIMEOUT_SEC="${AIPC_MCU_PREP_TIMEOUT_SEC:-115}"
PKG_PREFIX="ne503_ota_package_"

GREEN=$'\033[1;32m'; YELLOW=$'\033[1;33m'; RED=$'\033[1;31m'; NC=$'\033[0m'
ts()   { date '+%H:%M:%S'; }
log()  { printf '%s\n' "[aipc-mcu-prep] $(ts) $*"; }
info() { printf '%s\n' "[aipc-mcu-prep] $(ts) ${GREEN}INFO${NC} $*"; }
warn() { printf '%s\n' "[aipc-mcu-prep] $(ts) ${YELLOW}WARN${NC} $*"; }
err()  { printf '%s\n' "[aipc-mcu-prep] $(ts) ${RED}ERROR${NC} $*" >&2; }

rtc_result="skip"
ota_result="skip"

run_boot_prep() {
    local timeout_sec="$1"
    shift

    if [[ "$timeout_sec" =~ ^[0-9]+$ ]] && (( timeout_sec > 0 )) && command -v timeout >/dev/null 2>&1; then
        if timeout --help 2>&1 | grep -q -- '--kill-after'; then
            timeout --kill-after=5s "${timeout_sec}s" "$@"
        else
            timeout "${timeout_sec}s" "$@"
        fi
        return $?
    fi

    if ! command -v timeout >/dev/null 2>&1; then
        warn "timeout command not found; boot_prep is protected only by systemd TimeoutStartSec"
    fi
    "$@"
}

duplicate_skip() {
    warn "MCU prep already attempted this boot ($ONCE_MARKER); skipping duplicate start"
    log "complete (rtc=$rtc_result ota=$ota_result duplicate=skip)"
    exit 0
}

log "baseboard MCU boot prep starting"

if [[ ! "$BOOT_PREP_TIMEOUT_SEC" =~ ^[0-9]+$ ]] || (( BOOT_PREP_TIMEOUT_SEC < 1 )); then
    warn "invalid AIPC_MCU_PREP_TIMEOUT_SEC=$BOOT_PREP_TIMEOUT_SEC; using 115"
    BOOT_PREP_TIMEOUT_SEC=115
fi

# --- guard: binary must exist (bare image / pre-deploy) ---------------------
if [[ ! -x "$BIN" ]]; then
    warn "ne503_boot_prep not found at $BIN — skipping MCU prep (install the release first)"
    log "complete (rtc=$rtc_result ota=$ota_result)"
    exit 0
fi

# /run is per-boot, so this suppresses only duplicate starts in the same boot.
# Create it after the binary guard: a bare image may install the release later
# in the same boot and should still get one real attempt.
mkdir -p "$RUN_DIR" 2>/dev/null || true
if [[ "${AIPC_MCU_PREP_ONCE:-1}" != "0" ]]; then
    [[ -e "$ONCE_MARKER" ]] && duplicate_skip
    if ! (set -C; printf 'pid=%s start=%s\n' "$$" "$(date -u '+%Y-%m-%dT%H:%M:%SZ')" >"$ONCE_MARKER") 2>/dev/null; then
        [[ -e "$ONCE_MARKER" ]] && duplicate_skip
        warn "could not create per-boot marker $ONCE_MARKER; continuing without duplicate guard"
    fi
fi

# --- Phase 1: RTC — push host Linux UTC into the MCU RTC --------------------
# Only push when the host clock is plausible. aipc-firstboot has already run
# hwclock --hctosys (battery-backed RTC) by this point; if the battery RTC is
# empty / NTP has not converged, year < 2024 and we refuse to push bogus time.
args=(--serial /dev/ttyS0 --baud 921600)
year="$(date +%Y 2>/dev/null || echo 0)"
if [[ "$year" -ge 2024 ]]; then
    args+=(--rtc push-to-mcu)
    info "RTC: push host UTC to MCU (year=$year)"
else
    args+=(--rtc none)
    warn "RTC: host year=$year not sane (battery RTC empty / NTP pending); skip push"
fi

# --- Phase 2: firmware OTA — only when a package is staged ------------------
pkg=""
if [[ -d "$FW_DIR" ]]; then
    # version-sort so the highest-named package wins if several are staged.
    pkg="$(ls -1v "${FW_DIR}/${PKG_PREFIX}"*.bin 2>/dev/null | tail -n 1)"
fi
if [[ -n "$pkg" && -f "$pkg" ]]; then
    # ne503_ota_package_v0.1.5.bin -> 0.1.5
    ver="${pkg##*_v}"; ver="${ver%.bin}"
    if [[ "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+ ]]; then
        # upgrade-if=older: flash only when the MCU is strictly older than the
        # package; never re-flash an equal/newer image, so a normal boot is a
        # fast no-op once the MCU is current.
        args+=(--version-check on --firmware "$pkg" --expect-version "$ver" --upgrade-if older)
        info "OTA: package $(basename "$pkg") expect=$ver upgrade-if=older (no downgrade, no re-flash)"
    else
        args+=(--version-check off)
        warn "OTA: cannot parse version from $(basename "$pkg"); version-check off"
    fi
else
    args+=(--version-check off)
    info "OTA: no firmware package in $FW_DIR; version-check off"
fi

# --- invoke boot_prep (single run: RTC step first, then version/OTA) --------
info "running (timeout=${BOOT_PREP_TIMEOUT_SEC}s): $BIN ${args[*]}"
run_boot_prep "$BOOT_PREP_TIMEOUT_SEC" "$BIN" "${args[@]}"
rc=$?

case "$rc" in
    0)
        [[ "$year" -ge 2024 ]] && rtc_result="ok"
        if [[ -n "$pkg" && -f "$pkg" ]] && [[ "${args[*]}" == *"--version-check on"* ]]; then
            ota_result="done"
        fi
        info "boot_prep succeeded (rc=0)"
        ;;
    1) err "boot_prep param error (rc=1) — wrapper/arg bug; check $0" ;;
    2) warn "boot_prep MCU init failed (rc=2) — MCU not responding or /dev/ttyS0 busy; skipped this boot" ;;
    3) warn "boot_prep RTC step failed (rc=3) — MCU RTC not updated, will retry next boot" ;;
    4) warn "boot_prep firmware header check failed (rc=4) — bad or mismatched package: $pkg" ;;
    5) err "boot_prep OTA failed after 3 retries (rc=5) — MCU app unchanged, will retry next boot" ;;
    124|137) warn "boot_prep timed out after ${BOOT_PREP_TIMEOUT_SEC}s (rc=$rc) — killed to keep boot moving, will retry next boot" ;;
    *) warn "boot_prep unexpected exit rc=$rc" ;;
esac

log "complete (rtc=$rtc_result ota=$ota_result rc=$rc)"
exit 0
