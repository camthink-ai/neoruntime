#!/bin/bash
# Build the NE503 dev Docker image (no SDK baked in; mount it at runtime).
#
# Usage:
#   ./build.sh                                      # local image
#   ./build.sh org/ne503-dev-env:latest             # custom local image
#   PUSH=1 ./build.sh org/ne503-dev-env:latest      # buildx multi-arch push
#
# At runtime, mount the SDK:
#   docker run -it -v /opt/poky/4.0.23:/opt/hailo-sdk ne503-dev-env

set -euo pipefail

IMAGE_NAME="${1:-ne503-dev-env:latest}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
NODE_MAJOR="${NODE_MAJOR:-24}"
PNPM_VERSION="${PNPM_VERSION:-10.34.5}"
GOPROXY="${GOPROXY:-https://goproxy.cn,direct}"
PLATFORMS="${PLATFORMS:-linux/amd64,linux/arm64}"
PUSH="${PUSH:-0}"

echo "==> Building dev image (SDK mounted at runtime, not included)"
echo "    image:      $IMAGE_NAME"
echo "    node:       ${NODE_MAJOR}.x"
echo "    pnpm:       $PNPM_VERSION"
echo "    goproxy:    $GOPROXY"
if [ "$PUSH" = "1" ]; then
    echo "    platforms:  $PLATFORMS"
    docker buildx build \
        --platform "$PLATFORMS" \
        --build-arg "NODE_MAJOR=$NODE_MAJOR" \
        --build-arg "PNPM_VERSION=$PNPM_VERSION" \
        --build-arg "GOPROXY=$GOPROXY" \
        -f "$SCRIPT_DIR/Dockerfile.dev" \
        -t "$IMAGE_NAME" \
        --push \
        "$SCRIPT_DIR"
else
    docker build \
        --build-arg "NODE_MAJOR=$NODE_MAJOR" \
        --build-arg "PNPM_VERSION=$PNPM_VERSION" \
        --build-arg "GOPROXY=$GOPROXY" \
        -f "$SCRIPT_DIR/Dockerfile.dev" \
        -t "$IMAGE_NAME" \
        "$SCRIPT_DIR"
fi

echo ""
echo "==> Image built: $IMAGE_NAME"
if [ "$PUSH" != "1" ]; then
    echo "==> Push to Docker Hub:"
    echo "    PUSH=1 $0 $IMAGE_NAME"
fi
echo ""
echo "==> User workflow:"
echo "    docker pull $IMAGE_NAME"
echo "    docker run -it -v /opt/poky/4.0.23:/opt/hailo-sdk $IMAGE_NAME"
echo "    # Inside: git clone <repo> ~/ne503 && cd ~/ne503 && make pack-release VERSION=1.0.0"
