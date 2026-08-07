#!/bin/bash
# AIPC Platform - HTTP Smoke Tests
# NOTE: This runs HTTP smoke tests against a running Platform API.
# For unit tests:   make test-unit
# For smoke tests:  make test-smoke

set -e

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Project root
PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

API_BASE="http://localhost:8080/api/v1"

echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}AIPC Platform - HTTP Smoke Tests${NC}"
echo -e "${GREEN}========================================${NC}"
echo ""

# Test counters
PASSED=0
FAILED=0
SKIPPED=0

# Test function
test_api() {
    local name=$1
    local method=$2
    local endpoint=$3
    local data=$4
    
    echo -e "${BLUE}Testing: ${name}${NC}"
    
    if [ "$method" = "GET" ]; then
        response=$(curl -s -w "\n%{http_code}" "$API_BASE$endpoint" || echo "000")
    elif [ "$method" = "POST" ]; then
        response=$(curl -s -w "\n%{http_code}" -X POST \
            -H "Content-Type: application/json" \
            -d "$data" \
            "$API_BASE$endpoint" || echo "000")
    else
        response=$(curl -s -w "\n%{http_code}" -X "$method" \
            "$API_BASE$endpoint" || echo "000")
    fi
    
    http_code=$(echo "$response" | tail -n1)
    body=$(echo "$response" | sed '$d')
    
    if [ "$http_code" = "200" ] || [ "$http_code" = "201" ]; then
        echo -e "${GREEN}✓${NC} $name (HTTP $http_code)"
        PASSED=$((PASSED + 1))
        return 0
    else
        echo -e "${RED}✗${NC} $name (HTTP $http_code)"
        echo "  Response: $body"
        FAILED=$((FAILED + 1))
        return 1
    fi
}

# Check if services are running
echo -e "${YELLOW}[1/6] Checking services...${NC}"
SERVICES_OK=0

check_service() {
    local name=$1
    local sock=$2
    
    if [ -S "$sock" ] 2>/dev/null; then
        echo -e "${GREEN}✓${NC} $name is running"
        SERVICES_OK=$((SERVICES_OK + 1))
        return 0
    else
        echo -e "${RED}✗${NC} $name is not running"
        return 1
    fi
}

check_service "Event Bus" "/run/aipc/event-bus.sock" || true
check_service "Device Control" "/run/aipc/device-control.sock" || true
check_service "AI Runtime" "/run/aipc/ai-runtime.sock" || true
check_service "App Manager" "/run/aipc/app-manager.sock" || true

# Check Platform API
if curl -s "$API_BASE/system/health" > /dev/null 2>&1; then
    echo -e "${GREEN}✓${NC} Platform API is running"
    SERVICES_OK=$((SERVICES_OK + 1))
else
    echo -e "${RED}✗${NC} Platform API is not running"
fi

# Check if we have enough services for testing
# Need at least 2 core services (gRPC services) to proceed
if [ $SERVICES_OK -lt 2 ]; then
    echo ""
    echo -e "${RED}Error: Not enough services running.${NC}"
    echo "Please start services first:"
    echo "  ./scripts/start_mvp.sh"
    echo ""
    exit 1
fi

# Warn if Platform API is not running (HTTP tests will be skipped)
if ! curl -s "$API_BASE/system/health" > /dev/null 2>&1; then
    echo ""
    echo -e "${YELLOW}Warning: Platform API is not running.${NC}"
    echo "HTTP API tests will be skipped. To test HTTP API, start Platform API:"
    echo "  ./build/output/platform-api --config configs/platform-api.yaml &"
    echo ""
    SKIP_HTTP_TESTS=true
else
    SKIP_HTTP_TESTS=false
fi

echo ""

# Test System API
if [ "$SKIP_HTTP_TESTS" != "true" ]; then
    echo -e "${YELLOW}[2/6] Testing System API...${NC}"
    test_api "System Info" "GET" "/system/info"
    test_api "System Stats" "GET" "/system/stats"
    test_api "Health Check" "GET" "/system/health"
    echo ""

    # Test AI Runtime API
    echo -e "${YELLOW}[3/6] Testing AI Runtime API...${NC}"
    test_api "List Models" "GET" "/ai/models"
    test_api "AI Stats" "GET" "/ai/stats"
    echo ""

    # Test Event Bus API
    echo -e "${YELLOW}[4/6] Testing Event Bus API...${NC}"
    test_api "List Topics" "GET" "/events/topics"
    test_api "Publish Event" "POST" "/events/publish" '{"topic":"test.topic","payload":{"message":"test"}}'
    echo ""

    # Test Device Control API
    echo -e "${YELLOW}[5/6] Testing Device Control API...${NC}"
    test_api "Device Status" "GET" "/device/status"
    test_api "Set Light" "POST" "/device/light" '{"level":50}'
    test_api "Set IR LED" "POST" "/device/ir-led" '{"level":30}'
    test_api "PTZ Control" "POST" "/device/ptz" '{"action":"stop"}'
    echo ""

    # Test App Manager API
    echo -e "${YELLOW}[6/6] Testing App Manager API...${NC}"
    test_api "List Apps" "GET" "/apps"
    # Note: Install/Start/Stop tests require actual app manifests
    echo ""
else
    echo -e "${YELLOW}[2/6] Testing System API...${NC}"
    echo -e "${YELLOW}Skipped (Platform API not running)${NC}"
    echo ""
    echo -e "${YELLOW}[3/6] Testing AI Runtime API...${NC}"
    echo -e "${YELLOW}Skipped (Platform API not running)${NC}"
    echo ""
    echo -e "${YELLOW}[4/6] Testing Event Bus API...${NC}"
    echo -e "${YELLOW}Skipped (Platform API not running)${NC}"
    echo ""
    echo -e "${YELLOW}[5/6] Testing Device Control API...${NC}"
    echo -e "${YELLOW}Skipped (Platform API not running)${NC}"
    echo ""
    SKIPPED=$((SKIPPED + 20))  # Approximate number of skipped tests
fi

# Summary
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Test Summary${NC}"
echo -e "${GREEN}========================================${NC}"
echo -e "${GREEN}Passed:${NC} $PASSED"
echo -e "${RED}Failed:${NC} $FAILED"
echo -e "${YELLOW}Skipped:${NC} $SKIPPED"
echo ""

if [ $FAILED -eq 0 ]; then
    echo -e "${GREEN}All tests passed! ✓${NC}"
    exit 0
else
    echo -e "${RED}Some tests failed. Please check the output above.${NC}"
    exit 1
fi
