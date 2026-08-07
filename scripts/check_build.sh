#!/bin/bash
# Check build status and dependencies

# Don't exit on error - we want to report all missing deps
set +e

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

GREEN='\033[0;32m'
RED='\033[0;31m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Detect OS
OS_TYPE="unknown"
if [[ "$OSTYPE" == "msys" || "$OSTYPE" == "cygwin" || "$(uname -s)" == "MINGW"* ]]; then
    OS_TYPE="windows"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    OS_TYPE="linux"
elif [[ "$OSTYPE" == "darwin"* ]]; then
    OS_TYPE="macos"
fi

echo "=========================================="
echo "AIPC Platform - Build Status Check"
echo "=========================================="
echo ""

if [ "$OS_TYPE" == "windows" ]; then
    echo -e "${BLUE}Detected: Windows environment${NC}"
    echo -e "${YELLOW}Note: Some tools may need to be installed separately${NC}"
    echo ""
fi

# Check dependencies
echo "Checking dependencies..."
echo ""

MISSING_DEPS=0
MISSING_LIST=()

check_cmd() {
    local cmd=$1
    local install_hint=$2
    
    if command -v "$cmd" &> /dev/null; then
        local version_info=""
        case "$cmd" in
            go)
                version_info=$(go version 2>/dev/null | awk '{print $3}' || echo "")
                ;;
            cmake)
                version_info=$(cmake --version 2>/dev/null | head -1 | awk '{print $3}' || echo "")
                ;;
            protoc)
                version_info=$(protoc --version 2>/dev/null | awk '{print $2}' || echo "")
                ;;
            python3)
                version_info=$(python3 --version 2>/dev/null | awk '{print $2}' || echo "")
                ;;
        esac
        
        if [ -n "$version_info" ]; then
            echo -e "${GREEN}✓${NC} $cmd: $version_info ($(command -v $cmd))"
        else
            echo -e "${GREEN}✓${NC} $cmd: found ($(command -v $cmd))"
        fi
        return 0
    else
        echo -e "${RED}✗${NC} $cmd: not found"
        if [ -n "$install_hint" ]; then
            echo -e "   ${YELLOW}Hint: $install_hint${NC}"
        fi
        MISSING_DEPS=1
        MISSING_LIST+=("$cmd")
        return 1
    fi
}

# Check commands with installation hints
check_cmd go "Install from https://golang.org/dl/ or use: winget install GoLang.Go"
# Check Go version
if command -v go &> /dev/null; then
    GO_VERSION=$(go version | awk '{print $3}' | sed 's/go//')
    GO_MAJOR=$(echo $GO_VERSION | cut -d. -f1)
    GO_MINOR=$(echo $GO_VERSION | cut -d. -f2)
    if [ "$GO_MAJOR" -lt 1 ] || ([ "$GO_MAJOR" -eq 1 ] && [ "$GO_MINOR" -lt 21 ]); then
        echo -e "${YELLOW}⚠${NC} Go version $GO_VERSION is too old (requires 1.21+)"
        echo -e "   ${YELLOW}Please upgrade Go: https://golang.org/dl/${NC}"
    fi
fi
check_cmd cmake "Install from https://cmake.org/download/ or use: winget install Kitware.CMake"
check_cmd protoc "Install from https://grpc.io/docs/protoc-installation/ or use: winget install ProtocolBuffers.ProtocolBuffers"
check_cmd python3 "Install from https://www.python.org/downloads/ or use: winget install Python.Python.3"

if [ $MISSING_DEPS -eq 1 ]; then
    echo ""
    echo -e "${RED}Some dependencies are missing:${NC}"
    for cmd in "${MISSING_LIST[@]}"; do
        echo -e "  - ${RED}$cmd${NC}"
    done
    echo ""
    echo -e "${YELLOW}Installation options:${NC}"
    if [ "$OS_TYPE" == "windows" ]; then
        echo "  1. Use winget (Windows Package Manager):"
        echo "     winget install GoLang.Go"
        echo "     winget install Kitware.CMake"
        echo "     winget install ProtocolBuffers.ProtocolBuffers"
        echo "     winget install Python.Python.3"
        echo ""
        echo "  2. Download installers from official websites"
        echo "  3. Use Chocolatey: choco install golang cmake protoc python3"
    else
        echo "  Use your system package manager (apt, yum, brew, etc.)"
    fi
    echo ""
    echo -e "${YELLOW}Note: You can still check other build components below.${NC}"
    echo ""
fi

echo ""
echo "Checking Go modules..."
if [ -f go.mod ]; then
    echo -e "${GREEN}✓${NC} go.mod exists"
    if command -v go &> /dev/null; then
        # Use timeout to prevent hanging (5 seconds)
        if timeout 5 go mod verify 2>/dev/null; then
            echo -e "${GREEN}✓${NC} Go modules verified"
        else
            exit_code=$?
            if [ $exit_code -eq 124 ]; then
                echo -e "${YELLOW}⚠${NC} Go modules verification timed out (skipping)"
            else
                echo -e "${YELLOW}⚠${NC} Go modules need update"
            fi
        fi
    else
        echo -e "${YELLOW}⚠${NC} Cannot verify (Go not installed)"
    fi
else
    echo -e "${YELLOW}⚠${NC} go.mod not found (will be created on build)"
fi

echo ""
echo "Checking protobuf files..."
PROTO_COUNT=$(find platform -name "*.proto" 2>/dev/null | wc -l)
echo -e "${GREEN}✓${NC} Found $PROTO_COUNT .proto files"

# Check if proto files are compiled
COMPILED_COUNT=$(find platform -name "*.pb.go" 2>/dev/null | wc -l)
if [ $COMPILED_COUNT -gt 0 ]; then
    echo -e "${GREEN}✓${NC} Found $COMPILED_COUNT compiled .pb.go files"
else
    echo -e "${YELLOW}⚠${NC} No compiled .pb.go files found (run make proto)"
fi

echo ""
echo "Checking build outputs..."
BUILD_DIR="build/output"
if [ -d "$BUILD_DIR" ]; then
    echo -e "${GREEN}✓${NC} Build directory exists: $BUILD_DIR"
    
    # Count binaries
    BIN_COUNT=$(find "$BUILD_DIR" -type f -executable 2>/dev/null | wc -l)
    if [ $BIN_COUNT -gt 0 ]; then
        echo -e "${GREEN}✓${NC} Found $BIN_COUNT built binaries:"
        find "$BUILD_DIR" -type f -executable | while read bin; do
            echo "    - $(basename $bin)"
        done
    else
        echo -e "${YELLOW}⚠${NC} No binaries found (run make all)"
    fi
else
    echo -e "${YELLOW}⚠${NC} Build directory not found (will be created on build)"
fi

echo ""
echo "Checking HAL libraries..."
if [ -f "$BUILD_DIR/hal/stub/libaipc_hal.so" ] || [ -f "/opt/aipc/lib/hal/libaipc_hal.so" ]; then
    echo -e "${GREEN}✓${NC} HAL v2 library found"
else
    echo -e "${YELLOW}⚠${NC} HAL v2 library not found (run make hal-v2 HAL_PLATFORM=stub)"
fi

echo ""
echo "=========================================="
if [ $MISSING_DEPS -eq 1 ]; then
    echo -e "${YELLOW}Build status check complete (with warnings)${NC}"
    echo -e "${YELLOW}Please install missing dependencies to proceed with builds.${NC}"
else
    echo -e "${GREEN}Build status check complete!${NC}"
    echo -e "${GREEN}All dependencies are available.${NC}"
fi
echo "=========================================="
