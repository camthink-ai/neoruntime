#!/bin/bash
# Quick deploy script - detects the target architecture and pushes a build

set -e

TARGET_HOST="${1:-}"
TARGET_USER="${2:-root}"
TARGET_PREFIX="${INSTALL_PREFIX:-/data/aipc}"

if [ -z "$TARGET_HOST" ]; then
    echo "Usage: $0 <target-host> [username]"
    echo "Example: $0 192.168.1.100 root"
    exit 1
fi

echo "=== Quick deploy to $TARGET_USER@$TARGET_HOST ==="

# 1. Detect target architecture
echo "[1/5] Detecting target platform architecture..."
TARGET_ARCH=$(ssh "$TARGET_USER@$TARGET_HOST" "uname -m" 2>/dev/null || echo "unknown")
echo "Target architecture: $TARGET_ARCH"

# 2. Check the local build artifact architecture
LOCAL_ARCH=$(file build/output/ai-runtime 2>/dev/null | grep -oE "(x86-64|aarch64|ARM)" | head -1 || echo "unknown")
echo "Local build architecture: $LOCAL_ARCH"

# 3. If architectures differ, prompt for a cross build
if [ "$TARGET_ARCH" != "$LOCAL_ARCH" ] && [ "$TARGET_ARCH" != "unknown" ]; then
    echo "WARNING: architecture mismatch! A cross build is required"
    echo "Run the following, then re-run this script:"
    echo ""
    echo "  export GOOS=linux"
    case $TARGET_ARCH in
        aarch64|arm64)
            echo "  export GOARCH=arm64"
            ;;
        armv7l|armv6l)
            echo "  export GOARCH=arm"
            echo "  export GOARM=7"
            ;;
        x86_64|amd64)
            echo "  export GOARCH=amd64"
            ;;
    esac
    echo "  make clean && make platform"
    echo ""
    read -p "Continue deploying anyway (may fail)? [y/N] " -n 1 -r
    echo
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        exit 1
    fi
fi

# 4. Create the target directory tree
echo "[2/5] Creating target directories..."
ssh "$TARGET_USER@$TARGET_HOST" "mkdir -p $TARGET_PREFIX/{bin,lib/hal,etc,logs,data,models}"
ssh "$TARGET_USER@$TARGET_HOST" "mkdir -p /run/aipc/{shm,sockets}"

# 5. Deploy binaries
echo "[3/5] Deploying binaries..."
for bin in ai-runtime app-manager device-control event-bus platform-api; do
    if [ -f "build/output/$bin" ]; then
        scp "build/output/$bin" "$TARGET_USER@$TARGET_HOST:$TARGET_PREFIX/bin/" >/dev/null
        echo "  - $bin"
    fi
done

# Deploy HAL libraries
if [ -d "build/output/hal" ]; then
    ssh "$TARGET_USER@$TARGET_HOST" "mkdir -p $TARGET_PREFIX/lib/hal"
    find build/output/hal -type f \( -name "libaipc_hal*.so*" -o -name "libhal-*.so*" \) \
        -exec scp {} "$TARGET_USER@$TARGET_HOST:$TARGET_PREFIX/lib/hal/" \; 2>/dev/null || true
    echo "  - HAL libraries"
fi

# 6. Set permissions
echo "[4/5] Setting permissions..."
ssh "$TARGET_USER@$TARGET_HOST" "chmod +x $TARGET_PREFIX/bin/*"

# 7. Deploy configuration files
echo "[5/5] Deploying configuration files..."
scp -r configs/* "$TARGET_USER@$TARGET_HOST:$TARGET_PREFIX/etc/" >/dev/null 2>&1 || true

echo ""
echo "Deployment complete!"
echo ""
echo "Next steps:"
echo "  1. SSH to target: ssh $TARGET_USER@$TARGET_HOST"
echo "  2. Test a service: $TARGET_PREFIX/bin/ai-runtime -config $TARGET_PREFIX/etc/ai/ai-runtime.yaml"
echo "  3. View logs: tail -f $TARGET_PREFIX/logs/*.log"
