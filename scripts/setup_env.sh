#!/bin/bash
# setup_env.sh — NE503 build environment initialization
#
# Usage: ./scripts/setup_env.sh [layer1|layer2|layer3]
#   layer1 — Go, Node, pnpm, protoc, proto plugins (for make build-ci)
#   layer2 — layer1 + cmake, g++ (for make all)
#   layer3 — layer2 + Hailo SDK instructions (for make pack-release)
#
# Supported OS: Ubuntu/Debian (apt), macOS (brew)

set -e

LAYER="${1:-layer1}"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()  { echo -e "${GREEN}[INFO]${NC} $*"; }
warn()  { echo -e "${YELLOW}[WARN]${NC} $*"; }
error() { echo -e "${RED}[ERROR]${NC} $*"; exit 1; }

# ── OS detection ────────────────────────────────────────────────────────────

detect_pkg_manager() {
    if command -v apt-get >/dev/null 2>&1; then
        echo "apt"
    elif command -v brew >/dev/null 2>&1; then
        echo "brew"
    elif command -v dnf >/dev/null 2>&1; then
        echo "dnf"
    else
        echo "unknown"
    fi
}

PKG_MGR=$(detect_pkg_manager)

install_apt() {
    info "Installing via apt: $*"
    sudo apt-get update -qq
    sudo apt-get install -y -qq "$@"
}

install_brew() {
    info "Installing via brew: $*"
    brew install "$@"
}

pkg_install() {
    case "$PKG_MGR" in
        apt)  install_apt "$@" ;;
        brew) install_brew "$@" ;;
        dnf)  sudo dnf install -y "$@" ;;
        *)    error "Unsupported package manager. Install manually: $*" ;;
    esac
}

# ── Go ──────────────────────────────────────────────────────────────────────

ensure_go() {
    if command -v go >/dev/null 2>&1; then
        local ver=$(go version | awk '{print $3}' | sed 's/go//')
        info "Go $ver already installed"
        return
    fi

    info "Installing Go 1.25..."
    case "$PKG_MGR" in
        apt)
            # Add Go PPA for latest version
            sudo add-apt-repository -y ppa:longsleep/golang-backports 2>/dev/null || true
            install_apt golang-go
            ;;
        brew)
            install_brew go
            ;;
        *)
            warn "Install Go manually: https://go.dev/dl"
            return
            ;;
    esac
    go version
}

# ── Node.js ─────────────────────────────────────────────────────────────────

ensure_node() {
    if command -v node >/dev/null 2>&1; then
        info "Node.js $(node --version) already installed"
        return
    fi

    info "Installing Node.js 22..."
    case "$PKG_MGR" in
        apt)
            curl -fsSL https://deb.nodesource.com/setup_22.x | sudo -E bash -
            install_apt nodejs
            ;;
        brew)
            install_brew node
            ;;
        *)
            warn "Install Node.js manually: https://nodejs.org"
            return
            ;;
    esac
    node --version
}

# ── pnpm ────────────────────────────────────────────────────────────────────

ensure_pnpm() {
    if command -v pnpm >/dev/null 2>&1; then
        info "pnpm $(pnpm --version) already installed"
        return
    fi
    PNPM_VERSION="${PNPM_VERSION:-10.34.5}"
    info "Installing pnpm ${PNPM_VERSION}..."
    npm install -g "pnpm@${PNPM_VERSION}"
    pnpm --version
}

# ── Protobuf ────────────────────────────────────────────────────────────────

ensure_protoc() {
    if command -v protoc >/dev/null 2>&1; then
        info "protoc $(protoc --version) already installed"
        return
    fi

    info "Installing protoc..."
    case "$PKG_MGR" in
        apt)  install_apt protobuf-compiler ;;
        brew) install_brew protobuf ;;
        *)    warn "Install protoc manually: https://grpc.io/docs/protoc-installation/" ;;
    esac
}

ensure_proto_go_plugins() {
    if command -v protoc-gen-go >/dev/null 2>&1 && \
       command -v protoc-gen-go-grpc >/dev/null 2>&1; then
        info "protoc-gen-go and protoc-gen-go-grpc already installed"
        return
    fi

    info "Installing protoc Go plugins..."
    go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
    go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest

    # Ensure GOPATH/bin is in PATH
    local gopath=$(go env GOPATH)/bin
    case ":$PATH:" in
        *":$gopath:"*) ;;
        *)  echo "export PATH=\"\$PATH:$gopath\"" >> ~/.bashrc
            export PATH="$PATH:$gopath"
            info "Added $gopath to PATH (in ~/.bashrc)"
            ;;
    esac
}

# ── C/C++ toolchain ─────────────────────────────────────────────────────────

ensure_c_toolchain() {
    if command -v cmake >/dev/null 2>&1 && command -v g++ >/dev/null 2>&1; then
        info "cmake $(cmake --version | head -1 | awk '{print $3}') and g++ already installed"
        return
    fi

    info "Installing C/C++ toolchain..."
    case "$PKG_MGR" in
        apt)
            install_apt build-essential cmake
            ;;
        brew)
            install_brew cmake
            xcode-select --install 2>/dev/null || true
            ;;
        *)
            warn "Install cmake and g++ manually"
            ;;
    esac
}

ensure_grpc_cpp() {
    if command -v grpc_cpp_plugin >/dev/null 2>&1; then
        info "grpc_cpp_plugin already installed"
        return
    fi

    info "Installing gRPC C++ libraries..."
    case "$PKG_MGR" in
        apt)
            install_apt protobuf-compiler-grpc libgrpc++-dev libprotobuf-dev
            ;;
        brew)
            install_brew grpc
            ;;
        *)
            warn "Install gRPC C++ libraries manually"
            ;;
    esac
}

# ── Python ──────────────────────────────────────────────────────────────────

ensure_python() {
    if command -v python3 >/dev/null 2>&1; then
        info "Python $(python3 --version) already installed"
        return
    fi
    info "Installing Python 3..."
    case "$PKG_MGR" in
        apt)  install_apt python3 python3-pip ;;
        brew) install_brew python3 ;;
        *)    warn "Install Python 3 manually" ;;
    esac
}

# ── Layer installers ────────────────────────────────────────────────────────

install_layer1() {
    info "=== Setting up Layer 1: Universal build ==="
    ensure_go
    ensure_node
    ensure_pnpm
    ensure_protoc
    ensure_proto_go_plugins
    ensure_python
    info "=== Layer 1 ready ==="
    info "Run: make build-ci"
}

install_layer2() {
    install_layer1
    info "=== Setting up Layer 2: Native C/C++ build ==="
    ensure_c_toolchain
    ensure_grpc_cpp
    info "=== Layer 2 ready ==="
    info "Run: make all"
}

install_layer3() {
    install_layer2
    echo ""
    info "=== Layer 3: Hailo-15 cross-compile ==="
    info "Layer 3 requires manual installation of the Hailo Yocto SDK:"
    info ""
    info "  1. Download SDK from Hailo developer portal"
    info "  2. Install:  ./poky-glibc-x86_64-*-aarch64-toolchain-4.0.23.sh"
    info "  3. Source:   source /opt/poky/4.0.23/environment-setup-armv8a-poky-linux"
    info "  4. Verify:   echo \$CC  (should show aarch64-poky-linux-gcc)"
    info "  5. Build:    make hal-v2 HAL_PLATFORM=hailo15"
    info ""
    info "Also required on target device:"
    info "  - HailoRT runtime"
    info "  - Media Library"
    info "  - HAL bridge .so libraries"
}

# ── Main ────────────────────────────────────────────────────────────────────

case "$LAYER" in
    layer1) install_layer1 ;;
    layer2) install_layer2 ;;
    layer3) install_layer3 ;;
    *)      error "Unknown layer '$LAYER'. Usage: $0 [layer1|layer2|layer3]" ;;
esac
