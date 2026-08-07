#!/bin/bash
# Seccomp Profile Verification
# Verifies that a seccomp profile is correctly formatted and applied.

set -e

PROFILE_PATH="${1:-configs/security/seccomp-default.json}"
CONTAINER_ID="${2:-}"

echo "=== Seccomp Profile Verification ==="
echo ""

# 1. Verify the file exists
echo "1. Checking seccomp profile file..."
if [ ! -f "$PROFILE_PATH" ]; then
    echo "   ERROR: seccomp profile file not found: $PROFILE_PATH"
    exit 1
fi
echo "   OK: file exists: $PROFILE_PATH"

# 2. Validate JSON format
echo ""
echo "2. Validating JSON format..."
if ! python3 -m json.tool "$PROFILE_PATH" > /dev/null 2>&1; then
    echo "   ERROR: invalid JSON format"
    exit 1
fi
echo "   OK: valid JSON format"

# 3. Verify required fields
echo ""
echo "3. Verifying required fields..."
if ! grep -q '"defaultAction"' "$PROFILE_PATH"; then
    echo "   ERROR: missing defaultAction field"
    exit 1
fi
if ! grep -q '"syscalls"' "$PROFILE_PATH"; then
    echo "   ERROR: missing syscalls field"
    exit 1
fi
echo "   OK: required fields present"

# 4. Check architecture support
echo ""
echo "4. Checking architecture support..."
ARCH=$(uname -m)
case "$ARCH" in
    x86_64)
        ARCH_NAME="SCMP_ARCH_X86_64"
        ;;
    aarch64|arm64)
        ARCH_NAME="SCMP_ARCH_AARCH64"
        ;;
    armv7l|armv6l)
        ARCH_NAME="SCMP_ARCH_ARM"
        ;;
    *)
        echo "   WARNING: unknown architecture $ARCH, skipping arch check"
        ARCH_NAME=""
        ;;
esac

if [ -n "$ARCH_NAME" ]; then
    if grep -q "$ARCH_NAME" "$PROFILE_PATH"; then
        echo "   OK: supports current architecture: $ARCH ($ARCH_NAME)"
    else
        echo "   WARNING: profile may not support current architecture $ARCH"
    fi
fi

# 5. Verify seccomp in a running container (if a container ID is provided)
if [ -n "$CONTAINER_ID" ]; then
    echo ""
    echo "5. Verifying seccomp status in container..."

    # Check the container exists
    if ! crictl inspect "$CONTAINER_ID" > /dev/null 2>&1; then
        echo "   WARNING: cannot inspect container $CONTAINER_ID (may be using containerd instead of crictl)"
    else
        # Inspect seccomp state from the container process
        PID=$(crictl inspect "$CONTAINER_ID" | grep -o '"pid":[0-9]*' | head -1 | cut -d: -f2)
        if [ -n "$PID" ]; then
            SECCOMP_STATUS=$(grep "^Seccomp:" "/proc/$PID/status" | awk '{print $2}' 2>/dev/null || echo "unknown")
            if [ "$SECCOMP_STATUS" = "2" ]; then
                echo "   OK: Seccomp enabled (filter mode)"
            elif [ "$SECCOMP_STATUS" = "1" ]; then
                echo "   WARNING: Seccomp in strict mode (not filter)"
            else
                echo "   WARNING: Seccomp status: $SECCOMP_STATUS"
            fi
        fi
    fi
fi

# 6. Show profile summary
echo ""
echo "6. Profile summary:"
DEFAULT_ACTION=$(grep -o '"defaultAction"[[:space:]]*:[[:space:]]*"[^"]*"' "$PROFILE_PATH" | cut -d'"' -f4)
SYSCALL_COUNT=$(grep -c '"names"' "$PROFILE_PATH" || echo "0")
echo "   Default action: $DEFAULT_ACTION"
echo "   Syscall rule count: $SYSCALL_COUNT"

echo ""
echo "=== Verification complete ==="
echo ""
echo "Usage:"
echo "  $0 [profile_path] [container_id]"
echo ""
echo "Examples:"
echo "  $0 /etc/aipc/seccomp-default.json"
echo "  $0 /etc/aipc/seccomp-default.json aipc-hello-app"
