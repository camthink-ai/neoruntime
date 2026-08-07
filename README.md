# NE503 AIPC

[![CI](https://github.com/camthink-ai/ne503-aipc/actions/workflows/ci.yml/badge.svg)](https://github.com/camthink-ai/ne503-aipc/actions/workflows/ci.yml)
[![Release Package](https://github.com/camthink-ai/ne503-aipc/actions/workflows/release.yml/badge.svg)](https://github.com/camthink-ai/ne503-aipc/actions/workflows/release.yml)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

NE503 AIPC is the platform-core repository for CamThink's edge AI camera and
edge box stack. It contains platform services, HAL v2, deployment assets, MCU
firmware sources, and the web management console.

SDKs and sample applications are maintained separately:

- [`camthink-ai/ne503-aipc-sdks`](https://github.com/camthink-ai/ne503-aipc-sdks)
- [`camthink-ai/ne503-aipc-apps`](https://github.com/camthink-ai/ne503-aipc-apps)

## Quick Build

Use the full Docker image for the fastest Hailo-15 release build. The image
contains the Hailo/Poky SDK at `/opt/hailo-sdk`.

```bash
git clone https://github.com/camthink-ai/ne503-aipc.git
cd ne503-aipc

docker pull camthink/ne503-dev:v1.0
docker run --rm -it \
  --entrypoint /bin/bash \
  --user root \
  -v "$PWD:/ne503" \
  -w /ne503 \
  -e SDK_PATH=/opt/hailo-sdk \
  camthink/ne503-dev:v1.0
```

Inside the container:

```bash
git config --global --add safe.directory /ne503
make pack-release VERSION=0.1.0
```

The release package is written to `build/release/`. MCU OTA firmware is rebuilt
by default; use `BUILD_MCU_FW=0` only when packaging existing MCU artifacts.

## Architecture

```text
Application containers
  -> SDKs over gRPC, Unix sockets, and shared memory
Platform services
  -> platform-api, app-manager, event-bus, camera-daemon,
     ai-runtime, device-control, and discovery
Hardware abstraction
  -> HAL v2 common, DSP, media, model, and peripheral interfaces
Hardware and vendor runtimes
  -> Hailo-15, RK3588, Jetson, or stub backends
```

## Layout

| Path | Contents |
| ---- | -------- |
| `platform/` | Go and C++ platform services |
| `hal_v2/` | HAL v2 interfaces and backends |
| `mcu_board_prj/` | STM32G0 MCU firmware and OTA packaging |
| `web/` | React web console |
| `configs/` | Service configuration templates |
| `deploy/` | Runtime deployment assets |
| `systemd/` | System service units |
| `scripts/` | Build, deployment, and maintenance scripts |
| `tools/` | CLI and diagnostic tools |
| `docs/` | Project documentation |
| `tests/` | Unit and integration test assets |

## Local Build

Local builds require the Hailo/Poky SDK and host toolchains to be installed on
the build machine.

Native stub build:

```bash
make all
```

Fast validation:

```bash
make test-basic
make test
```

Common targets:

```bash
make build-go
make build-native
make build-web
make proto
make hal-v2 HAL_PLATFORM=stub
make pack VERSION=0.1.0
```

For a Hailo-15 release package, provide the vendor SDK path. MCU OTA firmware
is rebuilt by default:

```bash
make pack-release SDK_PATH=/opt/poky/4.0.23 VERSION=0.1.0
```

To package existing MCU artifacts without rebuilding them:

```bash
make pack-release SDK_PATH=/opt/poky/4.0.23 VERSION=0.1.0 BUILD_MCU_FW=0
```

Release packages are written to `build/release/` and are installed under
`/data/aipc/` on device.

Local build settings can be stored in gitignored `Makefile.local`:

```makefile
SDK_PATH=/opt/poky/4.0.23
HAL_PLATFORM=hailo15
```

## Requirements

- Go 1.25+
- CMake and GCC/G++
- protobuf and gRPC build tools
- Node.js 24 and pnpm 10 for the web console
- Docker for container builds
- Hailo/Poky SDK for local Hailo-15 release builds

## Configuration

Runtime secrets are not committed. Configure platform API authentication at
deployment time:

```bash
export AIPC_TOKEN_KEY="<random-signing-secret>"
export AIPC_AUTH_USERNAME="admin"
export AIPC_AUTH_PASSWORD="<strong-password>"
```

## Documentation

| Document | Purpose |
| -------- | ------- |
| [docs/README.md](docs/README.md) | Documentation index |
| [docs/getting-started/BUILD.md](docs/getting-started/BUILD.md) | Build guide |
| [docs/getting-started/QUICK_START.md](docs/getting-started/QUICK_START.md) | Quick start |
| [tests/TESTING_GUIDE.md](tests/TESTING_GUIDE.md) | Testing guide |
| [docs/api/swagger.yaml](docs/api/swagger.yaml) | API reference |
| [docs/architecture/README.md](docs/architecture/README.md) | Architecture overview |
| [docs/references/cli-guide.md](docs/references/cli-guide.md) | CLI guide |
| [docs/deployment/DEPLOYMENT.md](docs/deployment/DEPLOYMENT.md) | Deployment guide |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Contribution guide |

## License

This repository is licensed under the MIT License. See [LICENSE](LICENSE).
