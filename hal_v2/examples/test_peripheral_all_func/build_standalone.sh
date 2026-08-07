#!/usr/bin/env bash
# Standalone CMake project in this directory: links peripheral HAL sources into the binary so you do not
# need libaipc_hal.so / hal-*.so at runtime (system libs such as libgpiod still load from the rootfs).
#
# Cross-compile: pass a toolchain file or set OECORE_TARGET_SYSROOT, e.g.
#   export OECORE_TARGET_SYSROOT=/path/to/sysroots/armv8a-poky-linux
#   ./build_standalone.sh
# or:
#   ./build_standalone.sh -DCMAKE_TOOLCHAIN_FILE=/path/to/toolchain.cmake
#
# Environment:
#   BUILD_DIR  — build directory (default: ./build-standalone next to this script)

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${HERE}/build-standalone}"

cmake -S "${HERE}" -B "${BUILD_DIR}" "$@"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

OUT="${BUILD_DIR}/ne503_mcu_cli_tool"
echo ""
echo "Output: ${OUT}"
if command -v readelf >/dev/null 2>&1; then
    echo "Dynamic dependencies (should not list libaipc_hal.so):"
    readelf -d "${OUT}" 2>/dev/null | grep NEEDED || true
fi
