# Contributing to NE503 AIPC

Thanks for helping improve the NE503 AIPC platform.

## Repository Scope

This repository contains platform services, HAL v2 implementations, configuration
templates, deployment units, tools, tests, and the web console. SDKs and sample
applications are maintained in sibling repositories.

## Development Checks

Run the narrowest checks that cover your change:

```bash
make proto
make test-unit
make web
```

For C++ or HAL v2 changes, also run the relevant CMake target:

```bash
make hal-v2 HAL_PLATFORM=stub
make camera-daemon
```

## Pull Requests

- Keep changes focused and explain user-visible behavior.
- Do not commit secrets, customer data, model files, vendor SDKs, generated
  binaries, logs, build directories, or device-specific credentials.
- Prefer environment variables or gitignored local files for deployment
  secrets.
- Include tests or a clear manual verification note when automated coverage is
  not practical.
