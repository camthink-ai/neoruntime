#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SCRIPT="$ROOT/scripts/aipc-mcu-prep.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

FW_DIR="$TMP/firmware"
mkdir -p "$FW_DIR"

assert_contains() {
    local file="$1"
    local pattern="$2"
    if ! grep -qE "$pattern" "$file"; then
        echo "missing pattern '$pattern' in $file" >&2
        sed -n '1,160p' "$file" >&2
        exit 1
    fi
}

# A successful first run should create the per-boot marker; a second run in the
# same /run namespace must skip without invoking ne503_boot_prep again.
RUN_DIR="$TMP/run-once"
CALLS="$TMP/calls.log"
BIN="$TMP/ne503_boot_prep_ok"
cat >"$BIN" <<EOF
#!/bin/sh
echo "\$@" >>"$CALLS"
exit 0
EOF
chmod 0755 "$BIN"

AIPC_MCU_PREP_BIN="$BIN" \
AIPC_MCU_FW_DIR="$FW_DIR" \
AIPC_MCU_PREP_RUN_DIR="$RUN_DIR" \
AIPC_MCU_PREP_TIMEOUT_SEC=5 \
    bash "$SCRIPT" >"$TMP/first.out" 2>&1
assert_contains "$TMP/first.out" 'boot_prep succeeded \(rc=0\)'

AIPC_MCU_PREP_BIN="$BIN" \
AIPC_MCU_FW_DIR="$FW_DIR" \
AIPC_MCU_PREP_RUN_DIR="$RUN_DIR" \
AIPC_MCU_PREP_TIMEOUT_SEC=5 \
    bash "$SCRIPT" >"$TMP/second.out" 2>&1
assert_contains "$TMP/second.out" 'already attempted this boot'

if [[ "$(wc -l <"$CALLS")" -ne 1 ]]; then
    echo "expected one boot_prep invocation, got:" >&2
    cat "$CALLS" >&2
    exit 1
fi

# If ne503_boot_prep hangs, the wrapper must time it out before systemd's
# TimeoutStartSec and still exit 0 so boot can continue.
if command -v timeout >/dev/null 2>&1; then
    HANG_BIN="$TMP/ne503_boot_prep_hang"
    cat >"$HANG_BIN" <<'EOF'
#!/bin/sh
sleep 10
EOF
    chmod 0755 "$HANG_BIN"

    AIPC_MCU_PREP_BIN="$HANG_BIN" \
    AIPC_MCU_FW_DIR="$FW_DIR" \
    AIPC_MCU_PREP_RUN_DIR="$TMP/run-timeout" \
    AIPC_MCU_PREP_TIMEOUT_SEC=1 \
        bash "$SCRIPT" >"$TMP/timeout.out" 2>&1
    assert_contains "$TMP/timeout.out" 'boot_prep timed out after 1s'
    assert_contains "$TMP/timeout.out" 'complete .*rc=12[47]'
fi

echo "test_aipc_mcu_prep_wrapper: OK"
