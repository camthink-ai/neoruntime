#!/bin/bash
#
# Pre-generate protobuf/gRPC C++ code for ARM cross-compilation.
#
# Uses qemu-user-static to run the sysroot's ARM protoc + grpc_cpp_plugin
# so the generated code matches the target gRPC/protobuf version.
#
# Usage:
#   ./scripts/generate_proto_arm.sh [--sdk-path /path/to/hailo-sdk]
#
# Output:
#   platform/ai-runtime/proto_gen_arm/  (pre-generated .pb.cc/.pb.h/.grpc.pb.cc/.grpc.pb.h)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SDK_PATH="${HAILO_SDK_PATH:-/opt/poky/4.0.23}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --sdk-path) SDK_PATH="$2"; shift 2 ;;
        *) echo "Unknown arg: $1"; exit 1 ;;
    esac
done

SYSROOT="${SDK_PATH}/sysroots/armv8a-poky-linux"
PROTOC="${SYSROOT}/usr/bin/protoc"
GRPC_PLUGIN="${SYSROOT}/usr/bin/grpc_cpp_plugin"

if [[ ! -x "$PROTOC" ]]; then
    echo "Error: protoc not found at $PROTOC"
    exit 1
fi

HOST_ARCH="$(uname -m)"

# On arm64 hosts, bypass QEMU — use sysroot's dynamic linker directly.
# QEMU's -L sysroot isolation breaks on native arm64 (glibc mismatch).
# On x86_64 hosts, QEMU user-static correctly emulates the ARM binary.
if [[ "$HOST_ARCH" == "aarch64" ]]; then
    ARM_LD="${SYSROOT}/lib/ld-linux-aarch64.so.1"
    if [[ ! -x "$ARM_LD" ]]; then
        echo "Error: ARM dynamic linker not found at $ARM_LD"
        exit 1
    fi

    run_protoc() {
        "$ARM_LD" --library-path "$SYSROOT/lib:$SYSROOT/usr/lib" "$PROTOC" "$@"
    }

    # Wrapper: grpc_cpp_plugin also needs the sysroot loader
    PLUGIN_WRAPPER=$(mktemp)
    trap 'rm -f "$PLUGIN_WRAPPER"' EXIT
    cat > "$PLUGIN_WRAPPER" <<EOFSCRIPT
#!/bin/bash
exec "$ARM_LD" --library-path "$SYSROOT/lib:$SYSROOT/usr/lib" "$GRPC_PLUGIN" "\$@"
EOFSCRIPT
    chmod +x "$PLUGIN_WRAPPER"
else
    # x86_64 host — use QEMU user-static to run ARM protoc
    if ! command -v qemu-aarch64-static &>/dev/null; then
        echo "Error: qemu-aarch64-static not found. Install: apt install qemu-user-static"
        exit 1
    fi

    run_protoc() {
        qemu-aarch64-static -L "$SYSROOT" "$PROTOC" "$@"
    }

    PLUGIN_WRAPPER=$(mktemp)
    trap 'rm -f "$PLUGIN_WRAPPER"' EXIT
    cat > "$PLUGIN_WRAPPER" <<EOFSCRIPT
#!/bin/bash
exec qemu-aarch64-static -L "$SYSROOT" "$GRPC_PLUGIN" "\$@"
EOFSCRIPT
    chmod +x "$PLUGIN_WRAPPER"
fi

PROTO_DIR="$PROJECT_ROOT/platform/ai-runtime/proto"
OUT_DIR="$PROJECT_ROOT/platform/ai-runtime/proto_gen_arm"

mkdir -p "$OUT_DIR"

echo "Generating proto C++ (protoc=$(run_protoc --version 2>&1 | head -1))"

# Generate inference.proto from ai-runtime/proto
echo "  inference.proto ..."
run_protoc \
    --proto_path="$PROTO_DIR" \
    --cpp_out="$OUT_DIR" \
    --grpc_out="$OUT_DIR" \
    --plugin=protoc-gen-grpc="$PLUGIN_WRAPPER" \
    "$PROTO_DIR/inference.proto"

# Generate event.proto from event-bus/proto (separate directory for Go package separation)
EVENT_PROTO_DIR="$PROJECT_ROOT/platform/event-bus/proto"
echo "  event.proto ..."
run_protoc \
    --proto_path="$EVENT_PROTO_DIR" \
    --cpp_out="$OUT_DIR" \
    --grpc_out="$OUT_DIR" \
    --plugin=protoc-gen-grpc="$PLUGIN_WRAPPER" \
    "$EVENT_PROTO_DIR/event.proto"

# Generate camera.proto from camera-daemon/proto
CAMERA_PROTO_DIR="$PROJECT_ROOT/platform/camera-daemon/proto"
CAMERA_OUT_DIR="$PROJECT_ROOT/platform/camera-daemon/proto_gen_arm"
mkdir -p "$CAMERA_OUT_DIR"
echo "  camera.proto ..."
run_protoc \
    --proto_path="$CAMERA_PROTO_DIR" \
    --cpp_out="$CAMERA_OUT_DIR" \
    --grpc_out="$CAMERA_OUT_DIR" \
    --plugin=protoc-gen-grpc="$PLUGIN_WRAPPER" \
    "$CAMERA_PROTO_DIR"/*.proto

# Fix protobuf version mismatch in generated headers
# SDK has protoc 3.19.6 but libprotobuf.so.30 (3.20+), so remove version check
echo "  Patching protobuf version checks..."
for header in "$CAMERA_OUT_DIR"/*.pb.h; do
    if [[ -f "$header" ]]; then
        sed -i 's/^#if PROTOBUF_VERSION < 3019000$/#if PROTOBUF_VERSION < 3000000  \/* patched *\//' "$header"
        sed -i 's/^#if 3019006 < PROTOBUF_MIN_PROTOC_VERSION$/#if 0  \/* patched: version check disabled *\//' "$header"
    fi
done



echo ""
echo "Generated files:"
ls -la "$OUT_DIR"/*.cc "$OUT_DIR"/*.h 2>/dev/null
echo ""
echo "Done. Use -DPROTO_GEN_ARM_DIR=$OUT_DIR when cross-compiling."
