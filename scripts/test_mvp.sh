#!/bin/bash
# MVP Test Script - Test core functionality

set -e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
RED='\033[0;31m'
NC='\033[0m'

RUN_DIR="/run/aipc"

echo "=========================================="
echo "AIPC Platform - MVP Test"
echo "=========================================="
echo ""

# Check if services are running
echo -e "${BLUE}[1/4] Checking services...${NC}"
SERVICES_OK=0

check_service() {
    local name=$1
    local sock=$2
    
    if [ -S "$sock" ]; then
        echo -e "${GREEN}✓${NC} $name is running"
        SERVICES_OK=$((SERVICES_OK + 1))
        return 0
    else
        echo -e "${RED}✗${NC} $name is not running (socket not found: $sock)"
        return 1
    fi
}

check_service "Event Bus" "$RUN_DIR/event-bus.sock"
check_service "Device Control" "$RUN_DIR/device-control.sock"
check_service "AI Runtime" "$RUN_DIR/ai-runtime.sock"
check_service "App Manager" "$RUN_DIR/app-manager.sock"

if [ $SERVICES_OK -lt 2 ]; then
    echo ""
    echo -e "${RED}Error: Not enough services running. Start services first:${NC}"
    echo "  ./scripts/start_mvp.sh"
    exit 1
fi
echo ""

# Test Event Bus
echo -e "${BLUE}[2/4] Testing Event Bus...${NC}"
if [ -S "$RUN_DIR/event-bus.sock" ]; then
    echo -e "${GREEN}✓${NC} Event Bus socket exists"
    # TODO: Add actual gRPC test
else
    echo -e "${RED}✗${NC} Event Bus not available"
fi
echo ""

# Test Device Control
echo -e "${BLUE}[3/4] Testing Device Control...${NC}"
if [ -S "$RUN_DIR/device-control.sock" ]; then
    echo -e "${GREEN}✓${NC} Device Control socket exists"
    # TODO: Add actual gRPC test
else
    echo -e "${RED}✗${NC} Device Control not available"
fi
echo ""

# Test AI Runtime
echo -e "${BLUE}[4/4] Testing AI Runtime...${NC}"
if [ -S "$RUN_DIR/ai-runtime.sock" ]; then
    echo -e "${GREEN}✓${NC} AI Runtime socket exists"
    # TODO: Add actual gRPC test (ListModels, etc.)
else
    echo -e "${RED}✗${NC} AI Runtime not available"
fi
echo ""

echo "=========================================="
echo -e "${GREEN}MVP Test Complete!${NC}"
echo "=========================================="
echo ""
echo "Next steps:"
echo "  - Use Python SDK to interact with services"
echo "  - Check service logs: /tmp/aipc-*.log"
echo "  - Run unit tests: make test-unit"
echo ""
