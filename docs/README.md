# NeoRuntime Documentation

## Start Here

| Document | Description |
| -------- | ----------- |
| [getting-started/QUICK_START.md](getting-started/QUICK_START.md) | Local startup and first validation |
| [getting-started/BUILD.md](getting-started/BUILD.md) | Build targets and prerequisites |
| [architecture/README.md](architecture/README.md) | System architecture overview |
| [services/platform-api.md](services/platform-api.md) | Platform API service |
| [references/config-reference.md](references/config-reference.md) | Configuration reference |
| [deployment/DEPLOYMENT.md](deployment/DEPLOYMENT.md) | Deployment guide |

## Getting Started

| Document | Description |
| -------- | ----------- |
| [getting-started/QUICK_START.md](getting-started/QUICK_START.md) | Local startup and first validation |
| [getting-started/BUILD.md](getting-started/BUILD.md) | Build targets, layers, and prerequisites |
| [getting-started/MVP_GUIDE.md](getting-started/MVP_GUIDE.md) | Minimal MVP bring-up with `start_mvp.sh` |
| [getting-started/WINDOWS_SETUP.md](getting-started/WINDOWS_SETUP.md) | Windows development environment setup |

## Architecture

| Document | Description |
| -------- | ----------- |
| [architecture/README.md](architecture/README.md) | System architecture overview |
| [architecture/hal_v2_overview.md](architecture/hal_v2_overview.md) | HAL v2 design and extension points |
| [architecture/security-architecture.md](architecture/security-architecture.md) | Security model (auth, sandbox, seccomp) |
| [os-image-aipc-restore-design.md](deployment/os-image-aipc-restore-design.md) | OS image restore / `aipc-restore` design |

## Services

| Document | Description |
| -------- | ----------- |
| [services/ai-runtime.md](services/ai-runtime.md) | AI inference service |
| [services/app-manager.md](services/app-manager.md) | Application lifecycle management |
| [services/CAMERA_DAEMON_DESIGN.md](services/CAMERA_DAEMON_DESIGN.md) | Camera daemon design |
| [services/device-control.md](services/device-control.md) | Device/MCU control service |
| [services/device-discovery.md](services/device-discovery.md) | Device discovery service (CT-Disc) |
| [services/event-bus.md](services/event-bus.md) | Message pub/sub service |
| [services/media-streaming.md](services/media-streaming.md) | RTSP / media streaming |
| [services/platform-api.md](services/platform-api.md) | Platform API gateway service |
| [services/web-console.md](services/web-console.md) | Web console |

## References

| Document | Description |
| -------- | ----------- |
| [references/cli-guide.md](references/cli-guide.md) | `aipc-cli` command reference |
| [references/config-reference.md](references/config-reference.md) | All service configuration |
| [references/faq.md](references/faq.md) | Frequently asked questions |
| [references/hal-v2-api-reference.md](references/hal-v2-api-reference.md) | HAL v2 API reference |
| [references/SECCOMP_PROFILE_EXPLANATION.md](references/SECCOMP_PROFILE_EXPLANATION.md) | Seccomp profile explanation |
| [references/systemd-services.md](references/systemd-services.md) | systemd service units |
| [references/troubleshooting.md](references/troubleshooting.md) | General troubleshooting |
| [references/web-troubleshooting.md](references/web-troubleshooting.md) | Web console troubleshooting |

## Deployment & OS

| Document | Description |
| -------- | ----------- |
| [deployment/DEPLOYMENT.md](deployment/DEPLOYMENT.md) | Deployment guide (on-device release) |
| [deployment/YOCTO_DEPLOYMENT.md](deployment/YOCTO_DEPLOYMENT.md) | Yocto/Hailo-15 build & deploy |
| [os-upgrade.md](deployment/os-upgrade.md) | A/B OS upgrade design |
| [baseboard-mcu-rtc-ota.md](deployment/baseboard-mcu-rtc-ota.md) | Baseboard MCU RTC/OTA |

## Open Source & Release

| Document | Description |
| -------- | ----------- |
| [OPEN_SOURCE_SPLIT.md](OPEN_SOURCE_SPLIT.md) | Open-source split & pre-publish checklist (secrets, vendor assets, final gate) |

## Protocols

| Document | Description |
| -------- | ----------- |
| [references/CT_DISC_PROTOCOL.md](references/CT_DISC_PROTOCOL.md) | CT-Disc device discovery protocol spec |
| [mcu_protocol/README.md](mcu_protocol/README.md) | MCU communication protocol |
| [gyro-attitude-sse.md](references/gyro-attitude-sse.md) | Gyro attitude SSE stream contract |

## Benchmarks

| Document | Description |
| -------- | ----------- |
| [benchmarks/ai-model-benchmark-hailo15h.md](benchmarks/ai-model-benchmark-hailo15h.md) | AI model benchmarks (Hailo-15H) |
| [benchmarks/ai-runtime-performance-params.md](benchmarks/ai-runtime-performance-params.md) | AI runtime performance parameters |
| [benchmarks/npu-parallelism-benchmark.md](benchmarks/npu-parallelism-benchmark.md) | NPU parallelism benchmark |
| [benchmarks/video-decode-capability-assessment.md](benchmarks/video-decode-capability-assessment.md) | Video decode capability assessment |

## Testing & API

| Document | Description |
| -------- | ----------- |
| [testing/hal_lens_af0832_usage.md](testing/hal_lens_af0832_usage.md) | HAL lens (AF0832) usage |
| [api/swagger.yaml](api/swagger.yaml) | OpenAPI spec for the Platform API |
| [testing/lens_api_test.sh](testing/lens_api_test.sh) | Lens API test harness |

SDK and application guides live in the sibling repositories:

- `camthink-ai/neoruntime-sdks`
- `camthink-ai/neoruntime-apps`
