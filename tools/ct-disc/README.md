# CT-Disc device discovery and monitoring tool

`ct-disc` is a discovery, management, and resource-recording tool for NE503/AIPC devices. It has two entry points:

- CLI: command-line scan, listen, record, and MQTT command sending.
- GUI: a Windows desktop interface for scanning, manually adding devices, batch-recording CPU/memory/disk/NPU data, and viewing trend charts.

## Quick start

### Windows GUI

Current build output:

```text
tools/ct-disc/gui/ct-disc-gui/build/bin/ct-disc-gui.exe
```

Once opened, you can:

1. Click `Scan` to scan devices reachable via CT-Disc multicast.
2. Click `Add Device` to manually add cross-subnet devices, e.g. `192.168.1.100`.
3. Select devices and click `Record Data` to batch-record resource data.
4. Tick `One file per device` to generate one recording file per device, named by IP.

### Windows CLI

Current build output:

```text
tools/ct-disc/dist/ct-disc-windows-amd64.exe
```

Scan for devices:

```powershell
.\ct-disc-windows-amd64.exe scan --timeout 5 --count 3 -o json
```

Record a specific device:

```powershell
.\ct-disc-windows-amd64.exe record -a https://192.168.1.101 --token <TOKEN> --insecure-skip-tls-verify --interval 5s --samples 60 --one-file-per-device
```

## How discovery works

CT-Disc scanning uses UDP multicast:

```text
239.255.255.250:19850
```

Discovery depends on whether CT-Disc multicast packets are reachable, not simply on being on the same IP subnet. A device can respond to `ping` but still not be found by `scan` if multicast is unreachable; conversely, if multicast routing/forwarding is configured between subnets, devices can be scanned across subnets. If multicast is unreachable, use the GUI's `Add Device` or the CLI's `record -a` to specify the device IP/API URL directly.

## GUI features

### Scan devices

After clicking `Scan`, the GUI sends a CT-Disc probe and listens for device announcements. The device list shows:

- IP
- FW
- Product
- MAC
- SN
- Online status

If two devices share the same SN, recording files are not separated by SN — they are named by IP first to avoid overwriting each other.

### Manually add devices

Click `Add Device`; you can enter one IP or URL per line:

```text
192.168.1.100
https://192.168.1.101
```

When adding, you can configure:

- Protocol: `HTTP` or `HTTPS`
- API Port: defaults to `443` for HTTPS, `8080` for HTTP
- Username
- Token
- Skip HTTPS certificate verification

After a manual add, the GUI first shows a `Manual` device, then tries to enrich it from these endpoints:

```text
/api/v1/device-info
/api/v1/network/config
/api/v1/monitor/summary
```

Fields that can be enriched:

- SN: read from `serial_number` / `serialNumber` / `sn` / `SN` or `factory.serial_number`
- MAC: read from `mac_address` / `macAddress` / `mac` or `factory.mac_address`
- FW: read from `firmware_version` / `firmwareVersion` / `firmware` / `fw` / `version`
- Product / Model
- Hardware version

If an endpoint requires auth but no token was provided, the device is still added, but SN/FW/MAC may stay empty or show `manual-<ip>`.

### Record resource data

Click `Record Data` to configure:

- Format: `CSV` or `JSON Lines`
- Protocol / API Port
- Interval Seconds
- Samples
- Duration Minutes
- Username / Token
- Skip HTTPS certificate verification
- One file per device

Recording endpoints:

```text
/api/v1/monitor/summary
/api/v1/monitor/snapshot
```

Recorded fields include:

- `timestamp`
- `sn`
- `mac`
- `product`
- `ip`
- `api_url`
- `online`
- `metrics_ok`
- `error`
- `cpu_percent`
- `memory_percent`
- `disk_percent`
- `npu_percent`
- `temp_cpu`
- `temp_npu`
- `temp_board`
- `latency_ms`

`online` indicates whether the target exists as a recording target; `metrics_ok` indicates whether metrics were successfully read on this pass. To judge collection success, prefer `metrics_ok` and `error`.

### One file per device

With `One file per device` ticked, files are named by IP first:

```text
ct-disc-metrics_192-168-1-101.csv
ct-disc-metrics_192-168-1-100.csv
```

This avoids writing to the same file when SNs are duplicated.

### Trend charts

While recording, the GUI shows CPU, Memory, Disk, and NPU trends per device. Trends are built from successful records collected during this run, i.e. data points with `metrics_ok=true`.

## CLI usage

### scan

Actively send probes and wait for responses:

```bash
./ct-disc scan --timeout 5 --count 3
./ct-disc scan --timeout 5 --count 3 -o json
```

Common flags:

- `--timeout`: seconds to wait for responses
- `--count`: number of probes to send
- `--iface`: network interface name
- `-o, --output`: `table` / `json` / `yaml`

### list

Only listen for device announcements:

```bash
./ct-disc list --timeout 10
./ct-disc list --product NE503 --sn CT2026
```

### watch

Continuously listen for device online/offline and updates:

```bash
./ct-disc watch
./ct-disc watch --timeout 300
```

### record

Record resource data:

```bash
./ct-disc record -a http://192.168.1.101:8080 --interval 5s --samples 60 --file metrics.csv
```

Self-signed HTTPS certificates:

```bash
./ct-disc record -a https://192.168.1.101 --token <TOKEN> --insecure-skip-tls-verify --interval 5s --duration 1h
```

Record after auto-discovery:

```bash
./ct-disc record --scan-timeout 5s --count 3 --interval 10s --samples 360
```

One file per device:

```bash
./ct-disc record -a https://192.168.1.101 --token <TOKEN> --one-file-per-device
```

### send

Send a command to a device by SN over MQTT:

```bash
./ct-disc send CT2026-000812 reboot --broker tcp://192.168.1.102:1883 --payload '{}'
```

### announce

Simulate a CT-Disc announcement from a device side:

```bash
./ct-disc announce --product NE503 --sn CT2026-000812 --ip 192.168.1.101 --port 8080 --fw v1.3.8
```

## Build

### CLI

```bash
cd tools/ct-disc
make build
make build-all
make build-windows-amd64
```

### GUI

```bash
cd tools/ct-disc/gui/ct-disc-gui
wails build -platform windows/amd64 -clean
```

Or build the current-platform GUI from the `tools/ct-disc` directory:

```bash
make gui
```

## FAQ

### Can ping but not discovered by scan

Expected. `ping` is unicast, while CT-Disc scanning relies on multicast. Cross-subnet setups, VPN, WSL/virtual adapters, firewalls, and switch multicast policies can all block discovery; if cross-subnet multicast routing/forwarding is reachable, scanning can still work. Workarounds:

- Use the GUI's `Add Device` to add the IP manually.
- Use the CLI's `record -a <device URL>` to target it directly.
- For automatic cross-subnet discovery you need multicast routing/forwarding on the network, or run the scan tool on a network where multicast is reachable.

### online is true in the recording but there is no data

Check `metrics_ok`. For manually specified devices, `online=true` only means the device was added as a recording target; whether collection actually succeeded is judged by `metrics_ok` and `error`.

### SN/FW/MAC not shown after manual add

Usually the device API is unreachable, the protocol/port is wrong, or a token is missing. Check first:

- Whether `Skip HTTPS certificate verification` is ticked for HTTPS devices
- Whether the API Port is correct
- Whether the Token is correct
- Whether `/api/v1/device-info` or `/api/v1/network/config` returns an SN/MAC

### How long can I run a recording session

There is no hard limit; it mainly depends on the sampling interval, the number of devices, and disk space. Rough figures:

- 5-second interval: about a few MB to 10 MB per device per day
- 10-second interval: about 2.5 MB to 5 MB per device per day
- 60-second interval: about 0.5 MB to 1 MB per device per day

For batch soak testing, use a `10-30` second interval and tick `One file per device`.