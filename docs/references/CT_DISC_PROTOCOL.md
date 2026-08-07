# CT-Disc Protocol Specification

**Version**: 1.2.0
**Date**: 2026-06-03

---

## 1. Overview

CT-Disc (CamThink Device Discovery & Management) is a lightweight protocol for automatic device discovery and management on LAN/WAN networks. It supports two transport channels:

- **Multicast** (UDP) — same-subnet devices
- **MQTT** — cellular/cross-subnet devices

Core principle: **discovery channel determines management channel**.

---

## 2. Transport

### 2.1 UDP Multicast

| Parameter | Value |
|-----------|-------|
| Address | `239.255.255.250` |
| Port | `19850` |
| Broadcast interval | 5000ms |
| Max packet size | 1024 bytes |
| Transport | UDP |

### 2.2 MQTT

| Parameter | Value |
|-----------|-------|
| Register topic | `ct/disc/register` |
| Command topic | `ct/cmd/{sn}` |
| Response topic | `ct/resp/{sn}` |
| Register QoS | 0 |
| Command QoS | 1 |
| Register interval | 30000ms |

---

## 3. Message Types

### 3.1 ct-announce

Periodic broadcast sent by devices to announce their presence.

```json
{
    "type": "ct-announce",
    "product": "NE503",
    "sn": "CT503-2026-00001",
    "mac": "AA:BB:CC:DD:EE:FF",
    "ip": "192.168.1.100",
    "fw": "v1.2.0",
    "port": 8080,
    "hw": "Hailo-15",
    "caps": ["ai", "camera", "http", "mqtt"]
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | Must be `"ct-announce"` |
| `product` | string | Yes | Product model |
| `sn` | string | Yes | Serial number (primary key) |
| `mac` | string | No | MAC address |
| `ip` | string | Yes | Current IP address |
| `fw` | string | No | Firmware version |
| `port` | int | Yes | HTTP API port |
| `hw` | string | No | Hardware platform |
| `caps` | string[] | No | Capability list |

### 3.2 ct-register

MQTT registration from cellular/cross-subnet devices.

```json
{
    "type": "ct-register",
    "product": "NE101",
    "sn": "CT101-2026-00001",
    "mac": "AA:BB:CC:DD:EE:FF",
    "ip": "10.0.1.50",
    "fw": "v1.0.0",
    "port": 80,
    "hw": "ESP32-S3",
    "caps": ["camera", "mqtt", "http", "cellular"],
    "net": "cat1"
}
```

Same fields as `ct-announce`, plus:

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `net` | string | Yes | Network type: `cat1`, `wifi`, `eth`, `halow` |

### 3.3 ct-probe

Scanner-initiated probe to trigger immediate device responses.

```json
{
    "type": "ct-probe"
}
```

Devices receiving a `ct-probe` should immediately send a `ct-announce` response.

### 3.4 ct-set-network

Scanner-initiated network configuration command, sent via multicast. Works cross-subnet because multicast operates at L2.

```json
{
    "type": "ct-set-network",
    "sn": "CT503-2026-00001",
    "mac": "AA:BB:CC:DD:EE:FF",
    "interface": "eth0",
    "mode": "static",
    "ip_address": "192.0.2.100",
    "subnet_mask": "255.255.255.0",
    "gateway": "192.0.2.1",
    "dns1": "8.8.8.8",
    "dns2": "8.8.4.4"
}
```

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `type` | string | Yes | Must be `"ct-set-network"` |
| `sn` | string | Yes | Target device serial number |
| `mac` | string | No | Target MAC; when present it must match exactly and must not fall back to SN |
| `interface` | string | No | Network interface (default: `eth0`) |
| `mode` | string | Yes | `"dhcp"` or `"static"` |
| `ip_address` | string | Conditional | Required when `mode=static` |
| `subnet_mask` | string | Conditional | Required when `mode=static` |
| `gateway` | string | No | Default gateway |
| `dns1` | string | No | Primary DNS |
| `dns2` | string | No | Secondary DNS |

**Flow:**

```
Scanner                              Device
  │                                    │
  │── ct-set-network (multicast) ─────►│
  │                                    │ Check SN match
  │                                    │ Apply config
  │                                    │ Persist to /data/aipc/etc/network.yaml
  │◄── ct-announce (new IP) ──────────│
  │                                    │
```

DHCP mode example:
```json
{
    "type": "ct-set-network",
    "sn": "CT503-2026-00001",
    "interface": "eth0",
    "mode": "dhcp"
}
```

---

## 4. MQTT Management Commands

### 4.1 Command Format

**Topic: `ct/cmd/{sn}`**
```json
{
    "id": "cmd-20260603-001",
    "action": "reboot",
    "params": {},
    "timestamp": 1717401600
}
```

**Response Topic: `ct/resp/{sn}`**
```json
{
    "id": "cmd-20260603-001",
    "result": "ok",
    "data": {},
    "timestamp": 1717401601
}
```

### 4.2 Standard Actions

| Action | Description | Params | Typical Devices |
|--------|-------------|--------|-----------------|
| `reboot` | Reboot device | `{}` | All |
| `get_info` | Get device details | `{}` | All |
| `set_config` | Push configuration | `{key: value}` | All |
| `ota_upgrade` | OTA upgrade | `{"url": "..."}` | NE101, NE503 |
| `capture` | Trigger photo capture | `{}` | NE101 |
| `set_network` | Modify network config | `{"mode": "static"}` | NE503 |
| `list_apps` | List applications | `{}` | NE503 |
| `start_stream` | Start video stream | `{"type": "rtsp"}` | NE301, NE503 |

---

## 5. Device State Machine

```
ONLINE  ──(no message for 30s)──►  OFFLINE
OFFLINE ──(message received)────►  ONLINE
ONLINE  ──(periodic announce)───►  ONLINE (refresh last_seen)
```

| Parameter | Multicast | MQTT |
|-----------|-----------|------|
| Broadcast interval | 5s | 30s |
| Timeout threshold | 30s | 120s |
| Timeout check interval | 10s | 30s |

---

## 6. Standard Capability Identifiers (caps)

| Identifier | Description | Typical Devices |
|------------|-------------|-----------------|
| `ai` | Local AI inference | NE301, NE503 |
| `rtsp` | RTSP video streaming | NE301, NE503 |
| `mqtt` | MQTT connection | NE301, NE101 |
| `http` | HTTP API / Web | NE101, NE503 |
| `camera` | Camera capture | NE101 |
| `ptz` | PTZ control | NE503 |
| `lens` | Lens zoom/focus | NE503 |
| `gpio` | GPIO control | NE503 |
| `cellular` | Cellular network | NE101 |
| `halow` | HaLow (802.11ah) | NE101 |
| `lowpower` | Low power/sleep | NE101 |
| `ble` | Bluetooth | NE301 |

---

## 7. Product Defaults

| Product | caps | port | hw |
|---------|------|------|----|
| NE503 | `["ai","rtsp","ptz","lens","gpio"]` | 8080 | `Hailo-15` |
| NE301 | `["ai","rtsp","mqtt"]` | 80 | `STM32N6570` |
| NE101 (WiFi) | `["camera","mqtt","http"]` | 80 | `ESP32-S3` |
| NE101 (CAT1) | `["camera","mqtt","http","cellular"]` | 80 | `ESP32-S3` |
| NE101 (HaLow) | `["camera","mqtt","http","halow"]` | 80 | `ESP32-S3` |

---

## 8. Version History

| Version | Date | Changes |
|---------|------|---------|
| v1.0.0 | 2026-05-15 | Initial: ct-announce, ct-probe, multicast discovery, heartbeat timeout |
| v1.1.0 | 2026-05-20 | MQTT channel: ct-register, management commands, dual-transport |
| v1.2.0 | 2026-06-03 | ct-set-network: cross-subnet network config via multicast |

> **Note:** the earlier v1.1.0 draft also carried interaction scenarios and
> gRPC/REST API specifications. Those are maintained separately in
> [`../services/device-discovery.md`](../services/device-discovery.md) and
> `docs/api/swagger.yaml`; this file is the canonical wire-level protocol spec.
