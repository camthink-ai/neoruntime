#!/bin/bash
#
# AIPC Platform - Release Packaging (thin wrapper)
#
# Delegates to Makefile pack/pack-release targets.
# Build logic is in Makefile; this script provides the legacy CLI interface.
#
# Usage:
#   ./scripts/pack_release.sh --sdk-path /path/to/hailo-sdk
#   ./scripts/pack_release.sh --sdk-path /path/to/hailo-sdk --version 1.2.0
#   ./scripts/pack_release.sh --version test-1.0                    # stub build
#   ./scripts/pack_release.sh --sdk-path /path/to/sdk --skip-build  # repack only
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

# ---------- defaults ----------
SDK_PATH="${HAILO_SDK_PATH:-}"
VERSION=""
SKIP_BUILD=0
JOBS=$(nproc 2>/dev/null || echo 4)
CLEAN=0

# ---------- colors ----------
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

log()  { echo -e "${GREEN}[pack]${NC} $*"; }
warn() { echo -e "${YELLOW}[pack]${NC} $*"; }
err()  { echo -e "${RED}[pack]${NC} $*" >&2; }

# ---------- parse args ----------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-path)    SDK_PATH="$2"; shift 2 ;;
        --version|-v)  VERSION="$2";  shift 2 ;;
        --skip-build)  SKIP_BUILD=1;  shift ;;
        --clean)       CLEAN=1;       shift ;;
        --jobs|-j)     JOBS="$2";     shift 2 ;;
        -h|--help)
            cat <<'USAGE'
Usage: pack_release.sh [OPTIONS]

Options:
  --sdk-path PATH    Hailo SDK sysroot path (for ARM cross-compile)
  --version  VER     Release version tag (default: git describe or date)
  --skip-build       Skip build, repackage existing build/output
  --clean            Clean build directories before building
  --jobs N           Parallel build jobs (default: nproc)
  -h, --help         Show this help

Output:
  build/release/aipc-<platform>-<version>.tar.gz

Examples:
  ./scripts/pack_release.sh --version nx-1.0
  ./scripts/pack_release.sh --sdk-path /opt/poky/4.0.23 --version nx-1.0
USAGE
            exit 0
            ;;
        *) err "Unknown arg: $1"; exit 1 ;;
    esac
done

cd "$PROJECT_ROOT"

# ---------- version ----------
if [[ -z "$VERSION" ]]; then
    VERSION=$(git describe --tags --always --dirty 2>/dev/null || date +%Y%m%d-%H%M%S)
fi

log "============================================"
log "  AIPC Release Packaging"
log "  Version: $VERSION"
if [[ -n "$SDK_PATH" ]]; then
    log "  Mode:    Hailo-15 cross-compile"
    log "  SDK:     $SDK_PATH"
else
    log "  Mode:    Stub (native)"
fi
log "============================================"

# ---------- clean ----------
if [[ $CLEAN -eq 1 ]]; then
    log "Cleaning..."
    make clean
fi

# ---------- build + pack ----------
MAKE_ARGS=(VERSION="$VERSION" -j"$JOBS")

if [[ $SKIP_BUILD -eq 1 ]]; then
    log "Skipping build (--skip-build), repackaging existing build/output/"
    if [[ ! -d "$PROJECT_ROOT/build/output" ]]; then
        err "No existing build at build/output/ — run without --skip-build first."
        exit 1
    fi
    if [[ -n "$SDK_PATH" ]]; then
        make _pack-internal SDK_PATH="$SDK_PATH" HAL_PLATFORM=hailo15 "${MAKE_ARGS[@]}"
    else
        make _pack-stage "${MAKE_ARGS[@]}"
    fi
elif [[ -n "$SDK_PATH" ]]; then
    if [[ ! -d "$SDK_PATH" ]]; then
        err "SDK path not found: $SDK_PATH"
        exit 1
    fi
    make pack-release SDK_PATH="$SDK_PATH" "${MAKE_ARGS[@]}"
else
    make pack "${MAKE_ARGS[@]}"
fi
