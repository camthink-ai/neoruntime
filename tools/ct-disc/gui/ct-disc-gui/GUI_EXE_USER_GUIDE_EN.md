# CT-Disc GUI exe User Guide

Application:

```text
ct-disc-gui.exe
```

Default build path:

```text
tools/ct-disc/gui/ct-disc-gui/build/bin/ct-disc-gui.exe
```

## 1. Purpose

CT-Disc GUI discovers NE503/AIPC devices and records resource metrics from each device, including CPU, memory, disk, NPU, temperature, and request latency.

It supports two ways to add devices:

- `Scan`: discovers devices within the CT-Disc multicast-reachable network scope. Devices do not have to be in the same IP subnet as the PC; if multicast routing/relay works across subnets, they can also be discovered.
- `Add Device`: manually adds an IP address or URL. Use this when the device IP is reachable but multicast discovery is not.

## 2. Start the Application

On Windows, double-click:

```text
ct-disc-gui.exe
```

If Windows shows a security warning, allow the application to run. If a firewall prompt appears, allow network access.

## 3. Scan Devices

1. Select a network interface from the top dropdown, or keep `All Interfaces`.
2. Click `Scan`.
3. Wait for the scan to complete.
4. The device table shows `IP`, `FW`, `Product`, `MAC`, `SN`, and online status.

CT-Disc discovery uses UDP multicast:

```text
239.255.255.250:19850
```

Being able to `ping` a device does not guarantee that it can be discovered by scan. `ping` is unicast, while `Scan` depends on multicast. If multicast routing/relay is configured between subnets, cross-subnet devices can also be discovered. If multicast is not reachable, use `Add Device`.

## 4. Add Devices Manually

Click `Add Device` and enter one IP address or URL per line:

```text
192.168.1.100
https://192.168.1.101
```

Options:

- `Protocol`: choose `HTTPS` or `HTTP`.
- `API Port`: commonly `443` for HTTPS and `8080` for HTTP.
- `Username`: fill in if the device API requires it.
- `Token`: fill in if the device API requires authentication. Raw tokens and `Bearer <token>` are both accepted.
- `Skip HTTPS certificate verification`: enable this for devices using self-signed HTTPS certificates.

After adding a device, it first appears as a `Manual` device. The GUI then tries to load device information from:

```text
/api/v1/device-info
/api/v1/network/config
/api/v1/monitor/summary
```

If the API is reachable and authentication is correct, the table will show the real `SN`, `FW`, `MAC`, `Product`, and `HW`. If probing fails, the device remains in the list and may show `manual-<ip>`.

## 5. Device Details

Click a device row to open the detail window. It shows:

- Serial Number
- Product
- Firmware
- Hardware
- IP Address
- Port
- MAC Address
- First Seen
- Last Seen
- Capabilities

Manually added devices are marked as `Manual` and can be removed from the detail window.

## 6. Batch Selection

Use the checkboxes on the left side of the table:

- Select one device to include it in batch actions.
- Select the header checkbox to select all online devices.
- Click `Clear Selection` in the batch toolbar to clear the selection.

Selected devices can be recorded together with `Record Data`.

## 7. Record Resource Metrics

Select devices, then click `Record Data`.

Common settings:

- `Format`: `CSV` is recommended. `JSON Lines` is also supported.
- `Protocol` / `API Port`: must match the device API.
- `Interval Seconds`: sampling interval. Use `5` seconds for short debugging, and `10-30` seconds for long-running tests.
- `Samples`: number of samples. Use `0` for unlimited.
- `Duration Minutes`: recording duration. Use `0` for unlimited.
- `Username` / `Token`: used to access the device API.
- `One file per device`: recommended. It creates a separate file for each device.

Click `Start Recording` to start and `Stop Recording` to stop.

If both `Samples` and `Duration Minutes` are `0`, recording continues until manually stopped or the application is closed.

## 8. Output Files

The default output file is:

```text
ct-disc-metrics.csv
```

Use `Browse` to select another path.

When `One file per device` is enabled, files are named by IP first:

```text
ct-disc-metrics_192-168-1-101.csv
ct-disc-metrics_192-168-1-100.csv
```

This prevents file conflicts when multiple devices have the same SN.

## 9. Record Fields

Common CSV/JSON Lines fields:

- `timestamp`: sample time.
- `sn`: device serial number.
- `mac`: device MAC address.
- `product`: product/model name.
- `ip`: device IP address.
- `api_url`: API URL used for collection.
- `online`: whether the device is included as a recording target.
- `metrics_ok`: whether metrics were successfully collected in this sample.
- `error`: failure reason.
- `cpu_percent`: CPU usage.
- `memory_percent`: memory usage.
- `disk_percent`: disk usage.
- `npu_percent`: NPU usage.
- `temp_cpu` / `temp_npu` / `temp_board`: temperature values.
- `latency_ms`: request latency.

Use `metrics_ok` as the main indicator for collection success. For manually added devices, `online=true` only means the device is included as a target; it does not mean the monitoring API was successfully read.

## 10. Trend Charts

During recording, the lower part of the window shows per-device trend charts for:

- CPU
- Memory
- Disk
- NPU

Trend charts only use successful samples where `metrics_ok=true`.

## 11. Recommended Test Duration

The tool has no fixed recording time limit. The practical limit depends on sampling interval, device count, and disk space.

Reference estimates:

- 5-second interval: several MB to 10 MB per device per day.
- 10-second interval: about 2.5 MB to 5 MB per device per day.
- 60-second interval: about 0.5 MB to 1 MB per device per day.

Recommendations:

- Short debugging: 5-second interval, several hours to 1 day.
- Batch burn-in test: 10-30 second interval, 3-7 days.
- Long-term trend observation: 60-second interval, several weeks.

## 12. FAQ

### The device can be pinged but is not discovered

This is expected in some networks. `ping` is unicast, while CT-Disc discovery uses multicast. Cross-subnet routing, firewalls, VPNs, WSL virtual adapters, or switch multicast policies may block discovery.

If multicast routing/relay is configured between subnets and CT-Disc multicast packets can reach the PC, `Scan` can also discover cross-subnet devices.

Solutions:

- Use `Add Device` to manually add the device IP.
- Run the tool from the device network or another multicast-reachable location.
- Check that Windows Firewall allows the application to access the network.
- For cross-subnet auto discovery, check multicast routing, IGMP, ACLs, and switch policies.

### SN/FW/MAC is not shown after manual add

Common causes:

- Wrong protocol or port.
- Device API is not reachable.
- The device returns `401 Unauthorized`; a token is required.
- HTTPS uses a self-signed certificate and certificate verification was not skipped.

### metrics_ok=false in the record file

The sample failed. Check the `error` field in the same row. Common causes include timeout, connection refused, 401 unauthorized, or non-2xx HTTP status.

### Multiple devices have the same SN

Enable `One file per device`. Output files are named by IP first, so records will not overwrite each other.
