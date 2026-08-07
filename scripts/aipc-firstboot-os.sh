#!/bin/bash
# Stable OS-owned entry point. The full runtime script lives under the
# canonical /data/aipc root (persistent across rootfs replacement); this stub
# is baked into /usr/libexec so it survives even when /data/aipc is empty on a
# fresh rootfs. Legacy /data/scripts and /opt/aipc/scripts are kept as
# transition fallbacks for pre-migration devices.

set -euo pipefail

RUNTIME_FIRSTBOOT=/data/aipc/scripts/aipc-firstboot.sh
if [[ ! -x "${RUNTIME_FIRSTBOOT}" && -x /data/scripts/aipc-firstboot.sh ]]; then
    RUNTIME_FIRSTBOOT=/data/scripts/aipc-firstboot.sh
fi
if [[ ! -x "${RUNTIME_FIRSTBOOT}" && -x /opt/aipc/scripts/aipc-firstboot.sh ]]; then
    RUNTIME_FIRSTBOOT=/opt/aipc/scripts/aipc-firstboot.sh
fi
if [[ -x "${RUNTIME_FIRSTBOOT}" ]]; then
    exec /bin/bash "${RUNTIME_FIRSTBOOT}" "$@"
fi

echo "[aipc-firstboot] WARN: ${RUNTIME_FIRSTBOOT} is unavailable; applying minimal initialization"
mkdir -p /data/logs /data/health /data/core /data/journal /run/aipc /data/aipc/logs
chmod 1777 /data/core 2>/dev/null || true
modprobe hailo_integrated_nnc 2>/dev/null || true
modprobe hailo_pci 2>/dev/null || true
exit 0
