#!/usr/bin/env bash
# Standalone CMake build for ne503_boot_prep (links HAL sources into one binary).
#
# Cross-build (Yocto / Poky SDK): you MUST source the target environment first so
# CMake uses the SDK GCC (e.g. aarch64-poky-linux-gcc), not /usr/bin/gcc. Example:
#   source /path/to/sdk/environment-setup-armv8a-poky-linux
#   ./build_standalone.sh
# If you previously configured without the SDK, remove the build dir and re-run:
#   rm -rf build-standalone
#
# After sourcing, CROSS_COMPILE / OECORE_TARGET_SYSROOT are set; we pass them to
# CMake explicitly to avoid mixed host compiler + target sysroot (e.g. missing
# bits/timesize-32.h).

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="${BUILD_DIR:-${HERE}/build-standalone}"

CMAKE_EXTRA=()
if [ -n "${CROSS_COMPILE:-}" ] && command -v "${CROSS_COMPILE}gcc" >/dev/null 2>&1; then
    CMAKE_EXTRA+=(-DCMAKE_C_COMPILER="${CROSS_COMPILE}gcc" -DCMAKE_CXX_COMPILER="${CROSS_COMPILE}g++")
fi
if [ -n "${OECORE_TARGET_SYSROOT:-}" ]; then
    CMAKE_EXTRA+=(-DCMAKE_SYSROOT="${OECORE_TARGET_SYSROOT}")
fi

cmake -S "${HERE}" -B "${BUILD_DIR}" "${CMAKE_EXTRA[@]}" "$@"
cmake --build "${BUILD_DIR}" -j"$(nproc)"

echo ""
echo "Output: ${BUILD_DIR}/ne503_boot_prep"
