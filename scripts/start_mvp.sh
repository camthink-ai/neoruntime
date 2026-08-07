#!/bin/bash
# MVP Startup Script - Start all core services for MVP demo

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

BUILD_DIR="build/output"
RUN_DIR="/run/aipc"
SHM_DIR="/run/aipc/shm"

echo "=========================================="
echo "AIPC Platform - MVP Startup"
echo "=========================================="
echo ""

# Create runtime directories
echo -e "${BLUE}[1/5] Creating runtime directories...${NC}"
mkdir -p "$RUN_DIR"
mkdir -p "$SHM_DIR"
chmod 777 "$RUN_DIR" 2>/dev/null || true
chmod 777 "$SHM_DIR" 2>/dev/null || true

# Clean up old socket files
rm -f "$RUN_DIR"/*.sock 2>/dev/null || true
echo -e "${GREEN}✓${NC} Runtime directories ready"
echo ""

# Check if binaries exist
echo -e "${BLUE}[2/5] Checking binaries...${NC}"
if [ ! -d "$BUILD_DIR" ]; then
    echo -e "${RED}✗${NC} Build directory not found. Run make build-native first"
    exit 1
fi

BINARIES=(
    "ai-runtime"
    "event-bus"
    "device-control"
    "app-manager"
    "platform-api"
)

MISSING_BINS=()
for bin in "${BINARIES[@]}"; do
    if [ -f "$BUILD_DIR/$bin" ]; then
        echo -e "${GREEN}✓${NC} $bin found"
    else
        echo -e "${YELLOW}⚠${NC} $bin not found"
        MISSING_BINS+=("$bin")
    fi
done

if [ ${#MISSING_BINS[@]} -gt 0 ]; then
    echo ""
    echo -e "${YELLOW}Warning: Some binaries are missing. Building now...${NC}"
    make build-native || {
        echo -e "${RED}✗${NC} Build failed. Please fix build errors first."
        exit 1
    }
fi
echo ""

# Check HAL stub
echo -e "${BLUE}[3/5] Checking HAL stub library...${NC}"
HAL_STUB=""
if [ -f "$BUILD_DIR/hal/stub/libaipc_hal.so" ]; then
    HAL_STUB="$BUILD_DIR/hal/stub/libaipc_hal.so"
elif [ -f "/opt/aipc/lib/hal/libaipc_hal.so" ]; then
    HAL_STUB="/opt/aipc/lib/hal/libaipc_hal.so"
else
    echo -e "${YELLOW}⚠${NC} HAL stub not found. Building..."
    make hal-v2 HAL_PLATFORM=stub || {
        echo -e "${YELLOW}⚠${NC} HAL stub build failed. Services will use fallback mode."
    }
fi
if [ -n "$HAL_STUB" ]; then
    echo -e "${GREEN}✓${NC} HAL stub found: $HAL_STUB"
fi
echo ""

# Stop existing services
echo -e "${BLUE}[4/5] Stopping existing services...${NC}"
pkill -f "ai-runtime" 2>/dev/null || true
pkill -f "event-bus" 2>/dev/null || true
pkill -f "device-control" 2>/dev/null || true
pkill -f "app-manager" 2>/dev/null || true
pkill -f "platform-api" 2>/dev/null || true
sleep 1
echo -e "${GREEN}✓${NC} Cleaned up existing processes"
echo ""

# Start services
echo -e "${BLUE}[5/5] Starting services...${NC}"
echo ""

# Start Event Bus (first, as other services depend on it)
if [ -f "$BUILD_DIR/event-bus" ]; then
    echo -e "${GREEN}Starting Event Bus...${NC}"
    nohup "$BUILD_DIR/event-bus" \
        -config configs/platform/event-bus.yaml \
        > /tmp/aipc-event-bus.log 2>&1 &
    EVENT_BUS_PID=$!
    sleep 1
    if kill -0 $EVENT_BUS_PID 2>/dev/null; then
        echo -e "${GREEN}✓${NC} Event Bus started (PID: $EVENT_BUS_PID)"
    else
        echo -e "${RED}✗${NC} Event Bus failed to start. Check /tmp/aipc-event-bus.log"
    fi
fi

# Start Device Control
if [ -f "$BUILD_DIR/device-control" ]; then
    echo -e "${GREEN}Starting Device Control...${NC}"
    nohup "$BUILD_DIR/device-control" \
        -config configs/platform/device-control.yaml \
        > /tmp/aipc-device-control.log 2>&1 &
    DEVICE_PID=$!
    sleep 1
    if kill -0 $DEVICE_PID 2>/dev/null; then
        echo -e "${GREEN}✓${NC} Device Control started (PID: $DEVICE_PID)"
    else
        echo -e "${RED}✗${NC} Device Control failed to start. Check /tmp/aipc-device-control.log"
    fi
fi

# Start AI Runtime
if [ -f "$BUILD_DIR/ai-runtime" ]; then
    echo -e "${GREEN}Starting AI Runtime...${NC}"
    nohup "$BUILD_DIR/ai-runtime" \
        -config configs/ai/ai-runtime.yaml \
        > /tmp/aipc-ai-runtime.log 2>&1 &
    AI_RUNTIME_PID=$!
    sleep 1
    if kill -0 $AI_RUNTIME_PID 2>/dev/null; then
        echo -e "${GREEN}✓${NC} AI Runtime started (PID: $AI_RUNTIME_PID)"
    else
        echo -e "${RED}✗${NC} AI Runtime failed to start. Check /tmp/aipc-ai-runtime.log"
    fi
fi

# Start App Manager
if [ -f "$BUILD_DIR/app-manager" ]; then
    echo -e "${GREEN}Starting App Manager...${NC}"
    nohup "$BUILD_DIR/app-manager" \
        -config configs/platform/app-manager.yaml \
        > /tmp/aipc-app-manager.log 2>&1 &
    APP_MGR_PID=$!
    sleep 1
    if kill -0 $APP_MGR_PID 2>/dev/null; then
        echo -e "${GREEN}✓${NC} App Manager started (PID: $APP_MGR_PID)"
    else
        echo -e "${RED}✗${NC} App Manager failed to start. Check /tmp/aipc-app-manager.log"
    fi
fi

# Start Platform API (after all gRPC services are up)
if [ -f "$BUILD_DIR/platform-api" ]; then
    echo -e "${GREEN}Starting Platform API...${NC}"
    # Wait a bit for gRPC services to be ready
    sleep 2
    nohup "$BUILD_DIR/platform-api" \
        -config configs/platform-api.yaml \
        > /tmp/aipc-platform-api.log 2>&1 &
    PLATFORM_API_PID=$!
    sleep 2
    if kill -0 $PLATFORM_API_PID 2>/dev/null; then
        echo -e "${GREEN}✓${NC} Platform API started (PID: $PLATFORM_API_PID)"
    else
        echo -e "${RED}✗${NC} Platform API failed to start. Check /tmp/aipc-platform-api.log"
    fi
fi

echo ""
echo "=========================================="
echo -e "${GREEN}MVP Services Started!${NC}"
echo "=========================================="
echo ""
echo "Service Status:"
echo "  - Event Bus:      unix://$RUN_DIR/event-bus.sock"
echo "  - Device Control: unix://$RUN_DIR/device-control.sock"
echo "  - AI Runtime:     unix://$RUN_DIR/ai-runtime.sock"
echo "  - App Manager:    unix://$RUN_DIR/app-manager.sock"
if [ -n "$PLATFORM_API_PID" ] && kill -0 $PLATFORM_API_PID 2>/dev/null; then
    echo "  - Platform API:   http://localhost:8080"
fi
echo ""
echo "Logs:"
echo "  - Event Bus:      /tmp/aipc-event-bus.log"
echo "  - Device Control: /tmp/aipc-device-control.log"
echo "  - AI Runtime:     /tmp/aipc-ai-runtime.log"
echo "  - App Manager:    /tmp/aipc-app-manager.log"
if [ -n "$PLATFORM_API_PID" ] && kill -0 $PLATFORM_API_PID 2>/dev/null; then
    echo "  - Platform API:   /tmp/aipc-platform-api.log"
fi
echo ""
echo "To stop all services:"
echo "  ./scripts/stop_mvp.sh"
echo ""
echo "To test MVP:"
echo "  ./scripts/test_mvp.sh"
echo ""
