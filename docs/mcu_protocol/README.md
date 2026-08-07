# AIPC MCU Communication Protocol (host-link)

## Overview

The AIPC platform uses an STM32G0 MCU to control peripherals (LED, IR-cut, lens, RS-485,
alarm I/O, RTC, …). The Hailo-15 SoC talks to the MCU over a UART using the **host-link**
frame protocol. The wire format and command set are defined once in
`mcu_board_prj/app/Custom/Components/host_link/host_link_proto.h` and shared with the host
side via `hal_v2/common/host_link/` (identical source included in both builds).

## Transport

| Parameter | Value |
|-----------|-------|
| MCU UART | USART2 (STM32G0) |
| Baud rate | **921600** (`usart.c`), 8 data bits, no parity, 1 stop bit |
| MCU IO | DMA RX / DMA TX (`host_link_app.c`, `huart2`) |
| Max payload | 512 bytes (`HOST_LINK_CFG_MAX_PAYLOAD`) |
| Endianness | Little-endian on the wire |

## Frame Format

```
[ Header (14 bytes) ][ Payload: len bytes ][ payload_crc (2 bytes) ]
```

### Header (little-endian)

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0 | 2 | `magic` | Fixed `0x4C48` — `'H'`(0x48) `'L'`(0x4C) on the wire as little-endian uint16 (bytes `48 4C`) |
| 2 | 1 | `version` | Protocol version, currently `1` |
| 3 | 1 | `reserved` | Must be 0 |
| 4 | 2 | `frame_id` | Per-direction frame identifier (matches requests to responses) |
| 6 | 1 | `type` | `0`=REQUEST, `1`=RESPONSE, `2`=EVENT, `3`=EVENT_ACK |
| 7 | 1 | `reserved2` | Must be 0 |
| 8 | 2 | `cmd` | Command ID (see table below) |
| 10 | 2 | `len` | Payload length in bytes |
| 12 | 2 | `hdr_crc` | CRC16-CCITT over the first **12** header bytes (`magic`..`len`) |

### Trailer

| Field | Description |
|-------|-------------|
| `payload_crc` | 2-byte CRC16-CCITT over the payload bytes only (empty payload → CRC of zero-length input) |

### CRC16 (CCITT)

```c
/* init = 0xFFFF, poly = 0x1021, MSB-first */
uint16_t host_link_crc16(const uint8_t *data, uint16_t len) {
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8u; j++) {
            crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}
```

The receiver rejects a frame if `hdr_crc` or `payload_crc` do not match (`HOST_LINK_ERR_CRC`).

## Message Flow

- **REQUEST → RESPONSE**: the host sends a REQUEST with a `frame_id`; the MCU replies with a
  RESPONSE carrying the same `frame_id` and `cmd`. The host may register an async callback and
  retries up to `HOST_LINK_CFG_REQUEST_RETRY` (3) times on timeout (`HOST_LINK_CFG_ACK_TIMEOUT_MS`
  = 500 ms, poll slice 20 ms, send timeout 100 ms).
- **EVENT → EVENT_ACK**: the MCU pushes unsolicited EVENTS (alarm input, RS-485 RX, lens
  completion); the host replies with an EVENT_ACK. Pending events queue up to
  `HOST_LINK_CFG_NOTIFY_DEPTH` (16).

## Command IDs

Reserved / well-known (`0x0000–0x000F`) and NE503 MCU commands (`0x0010+`, BSP control + RTC + lens):

| Cmd | Name | Direction | Payload |
|-----|------|-----------|---------|
| `0x0000` | PING | H→M | — |
| `0x0001` | ECHO | H→M | arbitrary bytes |
| `0x0010` | GET_VERSION | H→M | — |
| `0x0011` | RTC_GET | H→M | — |
| `0x0012` | RTC_SET | H→M | `host_link_rtc_tm_t` (7B) |
| `0x0020` | LED_SET | H→M | `host_link_led_set_t` (2B: `led_id`, `duty`) |
| `0x0021` | LED_GET | H→M | — |
| `0x0022` | IRCUT_SET | H→M | `host_link_ch_enable_t` (2B: `channel`, `enable`) |
| `0x0023` | IRCUT_GET | H→M | — |
| `0x0024` | PD_GET | H→M | — |
| `0x0025` | TEMP_GET | H→M | — |
| `0x0026` | FAN_SET | H→M | `host_link_ch_enable_t` |
| `0x0027` | FAN_GET | H→M | — |
| `0x0028` | HEAT_SET | H→M | `host_link_ch_enable_t` |
| `0x0029` | HEAT_GET | H→M | — |
| `0x002A` | RADAR_SET | H→M | `host_link_ch_enable_t` |
| `0x002B` | RADAR_GET | H→M | — |
| `0x002C` | AOUT_SET | H→M | `host_link_ch_enable_t` |
| `0x002D` | AOUT_GET | H→M | — |
| `0x002E` | WOUT_SET | H→M | `host_link_ch_enable_t` |
| `0x002F` | WOUT_GET | H→M | — |
| `0x0030` | AIN_GET | H→M | — |
| `0x0031` | RESET_SOC | H→M | — |
| `0x0032` | RS485_INIT | H→M | `host_link_rs485_init_t` (7B: `baudrate`, `config[3]`) |
| `0x0033` | RS485_DEINIT | H→M | — |
| `0x0034` | RS485_TX | H→M | raw bytes |
| `0x0038` | LENS_INIT | H→M | — |
| `0x0039` | LENS_DEINIT | H→M | — |
| `0x003A` | LENS_CFG | H→M | `host_link_lens_cfg_t` (1B: `mode`) |
| `0x003B` | LENS_STATE_GET | H→M | — |
| `0x003C` | LENS_IRIS_RUN | H→M | `host_link_lens_motion_t` (6B) |
| `0x003D` | LENS_IRIS_STOP | H→M | — |
| `0x003E` | LENS_IRIS_TGT_SET | H→M | `host_link_lens_iris_tgt_t` (2B: `target`) |
| `0x003F` | LENS_IRIS_ADC_GET | H→M | — |
| `0x0040` | **EV_ALARM_IN** (event) | M→H | `host_link_alarm_in_evt_t` (2B: `channel`, `level`) |
| `0x0041` | **EV_RS485_RX** (event) | M→H | raw RS-485 RX bytes |
| `0x0042` | LENS_ZOOM_RUN | H→M | `host_link_lens_motion_t` |
| `0x0043` | LENS_ZOOM_ABS | H→M | `host_link_lens_motion_t` |
| `0x0044` | LENS_ZOOM_STOP | H→M | — |
| `0x0045` | LENS_ZOOM_RZ | H→M | — |
| `0x0046` | LENS_ZOOM_LIM_SET | H→M | `host_link_lens_limit_t` (8B) |
| `0x0047` | LENS_FOCUS_RUN | H→M | `host_link_lens_motion_t` |
| `0x0048` | LENS_FOCUS_ABS | H→M | `host_link_lens_motion_t` |
| `0x0049` | LENS_FOCUS_STOP | H→M | — |
| `0x004A` | LENS_FOCUS_RZ | H→M | — |
| `0x004B` | LENS_FOCUS_LIM_SET | H→M | `host_link_lens_limit_t` |
| `0x004C` | **EV_LENS** (event) | M→H | `host_link_lens_evt_t` (16B) |
| `0x004D` | LENS_ZF_SYNC_RUN | H→M | `host_link_lens_zf_sync_t` (12B) |
| `0x0050` | OTA_ENTER_BOOT | H→M | — |
| `0x0051` | REBOOT | H→M | — |

> `H→M` = host→MCU REQUEST, `M→H` = MCU→host EVENT. Many handlers reply with a bare
> `host_link_status_t` (4-byte `int32_t` status). ADC-family GETs (`PD_GET`, `TEMP_GET`,
> `AIN_GET`) reply with `host_link_adc_milli_t` (6B: `mv` + `milli`).

### OTA note

Single-slot XIP: the MCU **must not** download or write firmware over this link. The host sends
`OTA_ENTER_BOOT` (0x0050) to set a bootloader tag and reboot the MCU; the bootloader then
performs the Ymodem upgrade.

## Example: LED_SET request

```c
/* Payload: led_id=0 (near), duty=80 */
uint8_t payload[2] = { 0x00, 0x50 };
```

On-wire request:
```
48 4C 01 00 00 00 00 00 20 00 02 00 <hdr_crc:2> 00 50 <payload_crc:2>
```

Field-by-field: magic `48 4C`, version `01`, reserved `00`, frame_id `0000`, type `00`
(REQUEST), reserved2 `00`, cmd `2000` (LED_SET), len `0200`, then `hdr_crc` (CRC16 over the
preceding 12 bytes), payload `00 50`, then `payload_crc`.

## Implementation Reference

- **MCU side:** `mcu_board_prj/app/Custom/Components/host_link/` (core state machine +
  protocol), `mcu_board_prj/app/Custom/User/host_link_app.c` (USART2 + FreeRTOS task glue),
  `mcu_board_prj/app/Core/Src/usart.c` (921600 init).
- **Host side:** `hal_v2/common/host_link/` (same core sources compiled into the HAL),
  `hal_v2/platforms/hailo15/mcu/hailo15_mcu_impl.cpp` (serial open + baud mapping).
- **Wire definitions:** `mcu_board_prj/app/Custom/Components/host_link/host_link_proto.h`.

## Testing

- MCU console: `nr_micro_shell` on the debug console (`cmd_test.c`) exposes `host_link`
  commands for exercising the link from the MCU side.
- Host diagnostics: use a serial bridge on the Hailo-15 UART and send a `PING` (0x0000)
  REQUEST; expect a RESPONSE within the ACK timeout.
