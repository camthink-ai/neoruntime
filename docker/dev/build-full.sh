#!/bin/bash
# Build the NE503 full Docker image with the Hailo/Poky SDK baked in.
#
# Usage:
#   docker/dev/build-full.sh [SDK_PATH] [VERSION] [IMAGE_REPO]
#
# Examples:
#   docker/dev/build-full.sh /opt/poky/4.0.23 4.0.23 camthink/ne503-dev
#   PUSH=1 docker/dev/build-full.sh /opt/poky/4.0.23 4.0.23 camthink/ne503-dev
#
# Environment:
#   PUSH=1                         Build and push a multi-platform image
#   PLATFORMS=linux/amd64,linux/arm64
#   MAX_TRIES=3                    Retry count for push builds
#   POKY_VERSION=4.0.23            /opt/poky/<version> symlink inside image

set -euo pipefail

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
fi

SDK_PATH="${1:-/opt/poky/4.0.23}"
VERSION="${2:-$(basename "$SDK_PATH")}"
IMAGE_REPO="${3:-camthink/ne503-dev}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PUSH="${PUSH:-0}"
PLATFORMS="${PLATFORMS:-linux/amd64,linux/arm64}"
MAX_TRIES="${MAX_TRIES:-3}"
POKY_VERSION="${POKY_VERSION:-$(basename "$SDK_PATH")}"

[ -d "$SDK_PATH" ] || {
    echo "ERROR: SDK not found at $SDK_PATH" >&2
    exit 1
}

BUILD_ARGS=(
    --progress=plain
    -f "$SCRIPT_DIR/Dockerfile.dev-full"
    --build-context "sdk=$SDK_PATH"
    --build-arg "POKY_VERSION=$POKY_VERSION"
    -t "$IMAGE_REPO:$VERSION"
    -t "$IMAGE_REPO:latest"
)

echo "==> Building full dev image"
echo "    image:       $IMAGE_REPO:$VERSION"
echo "    latest:      $IMAGE_REPO:latest"
echo "    sdk:         $SDK_PATH"
echo "    poky:        $POKY_VERSION"
echo "    push:        $PUSH"

if [ "$PUSH" = "1" ]; then
    echo "    platforms:   $PLATFORMS"
    docker buildx use multiarch 2>/dev/null || docker buildx create --name multiarch --use --bootstrap

    for attempt in $(seq 1 "$MAX_TRIES"); do
        echo "==> buildx push attempt ${attempt}/${MAX_TRIES}"
        if docker buildx build \
            --platform "$PLATFORMS" \
            "${BUILD_ARGS[@]}" \
            --push \
            "$SCRIPT_DIR"; then
            echo "==> build and push succeeded"
            break
        fi

        if [ "$attempt" -eq "$MAX_TRIES" ]; then
            echo "ERROR: all ${MAX_TRIES} build/push attempts failed" >&2
            exit 1
        fi

        echo "==> attempt ${attempt} failed; retrying in 15s..." >&2
        sleep 15
    done

    echo "==> Remote manifest for $IMAGE_REPO:$VERSION"
    docker manifest inspect "$IMAGE_REPO:$VERSION" | grep -E '"architecture"|"os"' || true
else
    docker build "${BUILD_ARGS[@]}" "$SCRIPT_DIR"
    echo ""
    echo "==> Image built locally: $IMAGE_REPO:$VERSION"
    echo "==> Push multi-platform image:"
    echo "    PUSH=1 $0 $SDK_PATH $VERSION $IMAGE_REPO"
fi
