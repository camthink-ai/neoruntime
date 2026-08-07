#!/bin/bash
# Install AIPC Platform release into a Yocto rootfs (rootfs pre-seed).
# Consumes the newest staged release under build/release/ produced by:
#   make pack-release VERSION=<version>     (Hailo-15, requires the Yocto SDK)
#   make pack VERSION=<version>             (stub HAL, host arch)
# Usage: ./scripts/install_to_yocto.sh <rootfs_path>

set -e

ROOTFS_PATH="${1:-/tmp/rootfs}"

if [ ! -d "$ROOTFS_PATH" ]; then
    echo "Error: Rootfs path does not exist: $ROOTFS_PATH"
    exit 1
fi

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# Use the newest staged release tree, e.g. build/release/aipc-hailo15-1.0/
STAGE_DIR="$(ls -dt "${PROJECT_ROOT}/build/release"/aipc-* 2>/dev/null | head -1)"

if [ -z "$STAGE_DIR" ] || [ ! -d "$STAGE_DIR/opt/aipc" ]; then
    echo "Error: No staged release found under ${PROJECT_ROOT}/build/release/"
    echo "Please run: make pack-release VERSION=<version>   (or make pack VERSION=<version>)"
    exit 1
fi
BUILD_DIR="$STAGE_DIR"

echo "Installing AIPC Platform to: $ROOTFS_PATH"
echo "Source: $BUILD_DIR"
echo ""

# Install binaries
echo "[1/6] Installing binaries..."
install -d "$ROOTFS_PATH/usr/bin"
install -m 0755 "$BUILD_DIR/opt/aipc/bin"/* "$ROOTFS_PATH/usr/bin/" 2>/dev/null || true
echo "✓ Binaries installed"

# Install libraries
echo "[2/6] Installing libraries..."
install -d "$ROOTFS_PATH/usr/lib/aipc"
if [ -d "$BUILD_DIR/opt/aipc/lib" ]; then
    install -m 0644 "$BUILD_DIR/opt/aipc/lib"/* "$ROOTFS_PATH/usr/lib/aipc/" 2>/dev/null || true
fi
echo "✓ Libraries installed"

# Install configuration files
echo "[3/6] Installing configuration files..."
install -d "$ROOTFS_PATH/etc/aipc"
cp -r "$BUILD_DIR/opt/aipc/etc"/* "$ROOTFS_PATH/etc/aipc/" 2>/dev/null || true
echo "✓ Configuration files installed"

# Install systemd services
echo "[4/6] Installing systemd services..."
install -d "$ROOTFS_PATH/etc/systemd/system"
if [ -d "$BUILD_DIR/systemd" ]; then
    shopt -s nullglob
    units=("$BUILD_DIR"/systemd/*.service "$BUILD_DIR"/systemd/*.timer "$BUILD_DIR"/systemd/*.target)
    if [ ${#units[@]} -gt 0 ]; then
        install -m 0644 "${units[@]}" "$ROOTFS_PATH/etc/systemd/system/"
        echo "✓ Systemd units installed"
    else
        echo "⚠ No systemd units found in release, skipping"
    fi
else
    echo "⚠ Systemd directory not found in release, skipping"
fi

# Create runtime directories
echo "[5/6] Creating runtime directories..."
install -d "$ROOTFS_PATH/opt/aipc/logs"
install -d "$ROOTFS_PATH/opt/aipc/apps/registry"
install -d "$ROOTFS_PATH/opt/aipc/apps/instances"
install -d "$ROOTFS_PATH/run/aipc"
install -d "$ROOTFS_PATH/dev/shm"
echo "✓ Runtime directories created"

# Create symlinks for compatibility
echo "[6/6] Creating compatibility symlinks..."
install -d "$ROOTFS_PATH/opt/aipc/bin"
ln -sf /usr/bin/ai-runtime "$ROOTFS_PATH/opt/aipc/bin/ai-runtime"
ln -sf /usr/bin/app-manager "$ROOTFS_PATH/opt/aipc/bin/app-manager"
ln -sf /usr/bin/device-control "$ROOTFS_PATH/opt/aipc/bin/device-control"
ln -sf /usr/bin/event-bus "$ROOTFS_PATH/opt/aipc/bin/event-bus"
ln -sf /usr/bin/platform-api "$ROOTFS_PATH/opt/aipc/bin/platform-api"
ln -sf /usr/bin/aipc-cli "$ROOTFS_PATH/opt/aipc/bin/aipc-cli"
if [ -f "$ROOTFS_PATH/usr/bin/camera-daemon" ]; then
    ln -sf /usr/bin/camera-daemon "$ROOTFS_PATH/opt/aipc/bin/camera-daemon"
fi
ln -sf /etc/aipc "$ROOTFS_PATH/opt/aipc/etc"
echo "✓ Symlinks created"

echo ""
echo "=========================================="
echo "Installation complete!"
echo "=========================================="
echo ""
echo "Next steps:"
echo "1. Enable services in Yocto image:"
echo "   systemctl enable ai-runtime.service"
echo "   systemctl enable app-manager.service"
echo "   systemctl enable device-control.service"
echo "   systemctl enable event-bus.service"
echo "   systemctl enable platform-api.service"
echo ""
echo "2. Or add to your Yocto recipe:"
echo "   SYSTEMD_AUTO_ENABLE = \"enable\""
