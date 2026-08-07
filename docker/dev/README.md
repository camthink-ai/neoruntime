# NE503 Docker Development Images

This directory contains Dockerfiles and helper scripts for reproducible NE503
AIPC builds.

## Images

| Image | Dockerfile | SDK handling |
| ----- | ---------- | ------------ |
| `ne503-dev-env` | `Dockerfile.dev` | Mount the SDK at runtime |
| `ne503-dev-env-full` | `Dockerfile.dev-full` | Bake the SDK into the image |

Use the lightweight image for interactive development. Use the full image for
release builds and CI runners that should not depend on a host SDK mount.

## Build Modes

- Docker build: start a container, enter it, and run `make pack-release` inside
  the container.
- Local build: install the SDK and MCU toolchain on the host, then run
  `make pack-release SDK_PATH=/opt/poky/4.0.23` from the repository root.

## Build The Lightweight Image

```bash
docker/dev/build.sh camthink/ne503-dev:latest
```

Push a multi-platform image:

```bash
PUSH=1 docker/dev/build.sh camthink/ne503-dev:latest
```

Optional build settings:

```bash
NODE_MAJOR=24 PNPM_VERSION=10.34.5 docker/dev/build.sh camthink/ne503-dev:latest
GOPROXY=https://proxy.golang.org,direct docker/dev/build.sh camthink/ne503-dev:latest
```

Run it with a host SDK mount:

```bash
docker run --rm -it \
  -v /opt/poky/4.0.23:/opt/hailo-sdk:ro \
  -v "$PWD:/ne503" \
  -w /ne503 \
  camthink/ne503-dev:latest
```

Inside the container:

```bash
make pack-release VERSION=1.0.0
make pack-release VERSION=1.0.0 BUILD_MCU_FW=0
```

Use `BUILD_MCU_FW=0` only when packaging existing MCU artifacts.

## Persistent Development Container

The compose file starts a long-running container and persists its home
directory through a Docker volume:

```bash
docker compose -f docker/dev/docker-compose.dev.yml up -d
docker exec -it ne503-dev bash
```

To mount the host source tree directly:

```bash
docker compose -f docker/dev/docker-compose.dev.yml --profile mount up -d
docker exec -it ne503-dev-mount bash
```

If the SDK is not installed at `/opt/poky/4.0.23`, set the host path before
starting compose:

```bash
HOST_HAILO_SDK_PATH=/path/to/sdk docker compose -f docker/dev/docker-compose.dev.yml up -d
```

Stop the containers:

```bash
docker compose -f docker/dev/docker-compose.dev.yml down
```

## Build The Full Image

The full image copies the Hailo/Poky SDK into the image. The SDK is large, so
this build can take a while.

```bash
docker/dev/build-full.sh /opt/poky/4.0.23 4.0.23 camthink/ne503-dev
```

Push a multi-platform image:

```bash
PUSH=1 docker/dev/build-full.sh /opt/poky/4.0.23 4.0.23 camthink/ne503-dev
```

Run it without mounting the SDK:

```bash
docker run --rm -it \
  -v "$PWD:/ne503" \
  -w /ne503 \
  camthink/ne503-dev:v1.0
```

Inside the container:

```bash
make pack-release VERSION=1.0.0
make pack-release VERSION=1.0.0 BUILD_MCU_FW=0
```

Use `BUILD_MCU_FW=0` only when packaging existing MCU artifacts.

Outputs are written under `build/release/`.

## Notes

- Keep vendor SDKs out of the repository.
- The full image is intended for controlled build infrastructure because it
  contains the target SDK.
- Files created by container builds may be owned by root if the container is run
  as root.
