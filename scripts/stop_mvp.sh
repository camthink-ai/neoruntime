#!/bin/bash
# Stop all MVP services

GREEN='\033[0;32m'
YELLOW='\033[1;33m'
RED='\033[0;31m'
NC='\033[0m'

echo "Stopping AIPC Platform services..."

SERVICES=("ai-runtime" "event-bus" "device-control" "app-manager" "platform-api" "camera-daemon")

STOPPED=0
for service in "${SERVICES[@]}"; do
    PIDS=$(pgrep -f "$service" 2>/dev/null || true)
    if [ -n "$PIDS" ]; then
        echo "$PIDS" | xargs kill -TERM 2>/dev/null || true
        sleep 0.5
        # Force kill if still running
        REMAINING=$(pgrep -f "$service" 2>/dev/null || true)
        if [ -n "$REMAINING" ]; then
            echo "$REMAINING" | xargs kill -KILL 2>/dev/null || true
        fi
        echo -e "${GREEN}✓${NC} Stopped $service"
        STOPPED=$((STOPPED + 1))
    fi
done

if [ $STOPPED -eq 0 ]; then
    echo -e "${YELLOW}No services were running${NC}"
else
    echo ""
    echo -e "${GREEN}Stopped $STOPPED service(s)${NC}"
fi

