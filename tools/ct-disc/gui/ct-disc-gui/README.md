# CT-Disc GUI

CT-Disc GUI is a Windows desktop application for scanning NE503/AIPC devices and recording their resource data.

Full usage instructions:

```text
../../README.md
```

GUI exe English user guide:

```text
GUI_EXE_USER_GUIDE_EN.md
```

## Features

- Scan devices reachable via CT-Disc multicast
- Manually add cross-subnet devices by IP or URL
- Auto-fill SN, FW, MAC, Product, HW for manually added devices
- Batch device selection
- Record CPU, memory, disk, NPU, temperature, and request latency
- CSV / JSON Lines output
- One recording file per device, named by IP
- Per-device trend charts while recording

## Run in dev mode

```bash
cd tools/ct-disc/gui/ct-disc-gui
wails dev
```

## Build Windows GUI exe

```bash
cd tools/ct-disc/gui/ct-disc-gui
wails build -platform windows/amd64 -clean
```

Build output:

```text
build/bin/ct-disc-gui.exe
```

## Manual device enrichment

When adding a device you can configure the protocol, port, username, token, and HTTPS certificate verification options. The GUI attempts to read:

```text
/api/v1/device-info
/api/v1/network/config
/api/v1/monitor/summary
```

If an endpoint is reachable and authenticated correctly, the device list shows the real SN/FW/MAC. If the reads fail, the device is kept as `Manual` and recording still works.