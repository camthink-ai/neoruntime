#!/bin/sh
# Read-only A/B partition layout check for Hailo-15 NE503.

set -eu

DEVICE="${1:-${AIPC_FILESYSTEM_DEVICE:-mmcblk1}}"
case "$DEVICE" in
    /dev/*) ;;
    *) DEVICE="/dev/$DEVICE" ;;
esac

p1=0
p2=0
p3=0
p4=0
p5=0
for suffix in p1 p2 p3 p4 p5; do
    path="${DEVICE}${suffix}"
    if [ ! -b "$path" ]; then
        echo "MISSING: $path"
    else
        echo "OK:      $path"
        eval "$suffix=1"
    fi
done

echo
echo "Expected layout:"
echo "  copy A boot/rootfs: ${DEVICE}p1 ${DEVICE}p2"
echo "  copy B boot/rootfs: ${DEVICE}p3 ${DEVICE}p4"
echo "  persistent data:    ${DEVICE}p5"
echo
echo "Current root: $(findmnt -n -o SOURCE / 2>/dev/null || echo unknown)"
echo "Data mount:   $(findmnt -n -o SOURCE /data 2>/dev/null || echo not-mounted)"

mode=unsupported
if [ "$p1" -eq 1 ] && [ "$p2" -eq 1 ] && [ "$p3" -eq 1 ] &&
   [ "$p4" -eq 1 ] && [ "$p5" -eq 1 ]; then
    mode=dual
elif [ "$p1" -eq 1 ] && [ "$p2" -eq 1 ] && [ "$p3" -eq 1 ] &&
     [ "$p4" -eq 0 ] && [ "$p5" -eq 0 ]; then
    mode=single-recovery
fi

echo
case "$mode" in
    dual)
        echo "Upgrade mode: A/B dual-copy (online inactive-copy write + rollback)"
        ;;
    single-recovery)
        echo "Upgrade mode: legacy single-copy recovery"
        echo "WARNING: upgrade requires reboot, overwrites p1/p2, and has no automatic rollback."
        ;;
    *)
        echo "ERROR: unsupported partition layout."
        echo "Expected p1..p5 (dual) or p1..p3 only (single recovery)."
        exit 1
        ;;
esac

for path in "${DEVICE}p1" "${DEVICE}p2" "${DEVICE}p3" "${DEVICE}p4" "${DEVICE}p5"; do
    if findmnt -r -n -S "$path" >/dev/null 2>&1; then
        findmnt -r -n -S "$path" -o SOURCE,TARGET,FSTYPE
    fi
done

echo
echo "Supported OS upgrade partition layout is present."
