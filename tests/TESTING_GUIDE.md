# NE503 AIPC Testing Guide

## Quick Checks

```bash
make test-basic
make test-unit
```

## Integration Checks

Integration tests expect platform services to be running on their default Unix
domain sockets under `/run/aipc`.

```bash
make build-native
./scripts/start_mvp.sh
make test-integration
./scripts/stop_mvp.sh
```

## HAL v2 Stub Build

```bash
make hal-v2 HAL_PLATFORM=stub
```

## HTTP Smoke Tests

Start the platform API first, then run:

```bash
make test-smoke
```

Use synthetic local data only. Do not commit logs, captures, customer data, or
device credentials produced during validation.
