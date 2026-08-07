#!/usr/bin/env bash
# Basic read-only repository checks. Prefer `make test-basic`.

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$PROJECT_ROOT"

failed=0

check_dir() {
  if [ ! -d "$1" ]; then
    echo "Missing directory: $1" >&2
    failed=1
  fi
}

check_file() {
  if [ ! -f "$1" ]; then
    echo "Missing file: $1" >&2
    failed=1
  fi
}

required_dirs=(
  hal_v2/include
  hal_v2
  platform
  platform/common
  platform/ai-runtime
  platform/event-bus
  platform/device-control
  platform/app-manager
  platform/platform-api
  platform/device-discovery
  configs
  scripts
  tests
)

for path in "${required_dirs[@]}"; do
  check_dir "$path"
done

required_files=(
  go.mod
  Makefile
  hal_v2/include/media/hal_video.h
  hal_v2/include/model/hal_inference.h
  hal_v2/include/media/hal_codec.h
  hal_v2/include/peripheral/hal_io.h
  platform/ai-runtime/proto/inference.proto
  platform/event-bus/proto/event.proto
  platform/device-control/proto/device.proto
  platform/app-manager/proto/app.proto
  platform/camera-daemon/proto/camera.proto
  platform/device-discovery/proto/discovery.proto
)

for path in "${required_files[@]}"; do
  check_file "$path"
done

if command -v gofmt >/dev/null 2>&1; then
  unformatted="$(gofmt -l platform tests/unit 2>/dev/null || true)"
  if [ -n "$unformatted" ]; then
    echo "Go files need formatting:" >&2
    echo "$unformatted" >&2
    failed=1
  fi
else
  echo "gofmt not found" >&2
  failed=1
fi

if command -v go >/dev/null 2>&1; then
  go list -mod=mod ./platform/... >/dev/null || failed=1
else
  echo "go not found" >&2
  failed=1
fi

if [ "$failed" -eq 0 ]; then
  echo "Basic checks passed."
else
  echo "Basic checks failed." >&2
fi

exit "$failed"
