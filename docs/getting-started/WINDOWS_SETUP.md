# Windows Development Setup Guide

This guide helps you set up the AIPC Platform development environment on Windows.

---

## Prerequisites

### Required Tools

1. **Git Bash** (or WSL2)
   - Download from: https://git-scm.com/downloads
   - Or use WSL2: `wsl --install`

2. **Go** (1.21+)
   - Download: https://golang.org/dl/
   - Or use winget: `winget install GoLang.Go`
   - Verify: `go version`

3. **CMake** (3.20+)
   - Download: https://cmake.org/download/
   - Or use winget: `winget install Kitware.CMake`
   - Verify: `cmake --version`

4. **Protocol Buffers Compiler** (protoc)
   - Download: https://github.com/protocolbuffers/protobuf/releases
   - Or use winget: `winget install ProtocolBuffers.ProtocolBuffers`
   - Verify: `protoc --version`

5. **Python 3** (3.8+)
   - Download: https://www.python.org/downloads/
   - Or use winget: `winget install Python.Python.3`
   - Verify: `python3 --version`

### Optional Tools

- **Chocolatey** (Package Manager)
  - Install: https://chocolatey.org/install
  - Then: `choco install golang cmake protoc python3`

- **Visual Studio Build Tools** (for C++ compilation)
  - Download: https://visualstudio.microsoft.com/downloads/
  - Install "Desktop development with C++" workload

---

## Quick Installation (Windows)

### Using winget (Recommended)

```powershell
# Open PowerShell as Administrator
winget install GoLang.Go
winget install Kitware.CMake
winget install ProtocolBuffers.ProtocolBuffers
winget install Python.Python.3
```

### Using Chocolatey

```powershell
# Open PowerShell as Administrator
choco install golang cmake protoc python3
```

### Manual Installation

1. Download installers from official websites
2. Add to PATH during installation
3. Restart Git Bash/PowerShell after installation

---

## Environment Setup

### 1. Verify Installation

Open Git Bash and run:

```bash
./scripts/check_build.sh
```

This will check all dependencies and report what's missing.

### 2. Set Up Go Environment

```bash
# Add to ~/.bashrc or ~/.bash_profile
export GOPATH=$HOME/go
export PATH=$PATH:/usr/local/go/bin:$GOPATH/bin
```

### 3. Install Go Tools

```bash
# Install protobuf Go plugins
go install google.golang.org/protobuf/cmd/protoc-gen-go@latest
go install google.golang.org/grpc/cmd/protoc-gen-go-grpc@latest

# Add to PATH
export PATH=$PATH:$(go env GOPATH)/bin
```

---

## Building on Windows

### Option 1: Git Bash

```bash
# Check build status
./scripts/check_build.sh

# Build all components
make all

# Build specific component
make ai-runtime
```

### Option 2: WSL2 (Recommended for C++)

If you need to build C++ components (camera-daemon, HAL), WSL2 is recommended:

```bash
# In WSL2
cd /mnt/f/projects/ne503
make all
```

### Option 3: Visual Studio

For C++ development, you can use Visual Studio:
1. Open `platform/camera-daemon/CMakeLists.txt` in Visual Studio
2. Configure and build

---

## Common Issues

### Issue: "go: not found"

**Solution:**
1. Install Go from https://golang.org/dl/
2. Add Go to PATH:
   - Windows: Add `C:\Program Files\Go\bin` to System PATH
   - Git Bash: Add to `~/.bashrc`:
     ```bash
     export PATH=$PATH:/c/Program\ Files/Go/bin
     ```
3. Restart terminal

### Issue: "protoc: not found"

**Solution:**
1. Download protoc from https://github.com/protocolbuffers/protobuf/releases
2. Extract and add `bin` directory to PATH
3. Or use: `winget install ProtocolBuffers.ProtocolBuffers`

### Issue: "cmake: not found"

**Solution:**
1. Install CMake from https://cmake.org/download/
2. During installation, select "Add CMake to system PATH"
3. Or use: `winget install Kitware.CMake`

### Issue: C++ Build Fails

**Solution:**
1. Install Visual Studio Build Tools
2. Or use WSL2 for Linux-like environment
3. Or skip C++ components (HAL stubs, camera-daemon) for now

---

## Development Workflow

### 1. Check Environment

```bash
./scripts/check_build.sh
```

### 2. Initialize Go Modules

```bash
go mod init github.com/aipc/platform
go mod tidy
```

### 3. Compile Protobuf

```bash
make proto
```

### 4. Build Services

```bash
# Build all
make all

# Or individual
make ai-runtime
make device-control
make event-bus
make platform-app-manager
```

### 5. Run Tests

```bash
# Unit tests
go test ./platform/...

# Integration tests
cd tests/integration
go test -v ./...
```

---

## Windows-Specific Notes

### Path Separators

- Git Bash uses forward slashes: `/f/projects/ne503`
- Windows uses backslashes: `F:\projects\ne503`
- Scripts handle both, but prefer forward slashes in Git Bash

### Line Endings

- Git should auto-convert CRLF to LF
- If issues occur: `git config core.autocrlf true`

### Executable Permissions

- Windows doesn't use Unix permissions
- Scripts should still be executable in Git Bash
- Use: `chmod +x scripts/*.sh`

### Shared Libraries

- HAL libraries use `.so` extension (Linux)
- On Windows, you may need `.dll` versions
- For now, focus on Go services (which work cross-platform)

---

## Next Steps

1. ✅ Install all dependencies
2. ✅ Run `./scripts/check_build.sh` to verify
3. ✅ Build Go services: `make platform`
4. ✅ Run tests: `go test ./...`
5. 🔄 For C++ components, consider using WSL2

---

## Troubleshooting

### Check PATH

```bash
echo $PATH
which go
which cmake
which protoc
```

### Verify Go Installation

```bash
go env
go version
```

### Test Protobuf

```bash
protoc --version
protoc-gen-go --version
protoc-gen-go-grpc --version
```

---

## Support

If you encounter issues:

1. Check `./scripts/check_build.sh` output
2. Verify all tools are in PATH
3. Restart terminal after installing tools
4. Check Windows Event Viewer for errors
5. Try WSL2 for Linux-like environment

---

**Last Updated:** 2024-12-12
