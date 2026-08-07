# NE503 MCU CLI tool

This document describes the `test_peripheral_all_func` example: an interactive serial CLI that exercises HAL V2 MCU / I/O / peripheral APIs.

## Invocation

```text
<executable> <serial_device> [baud_rate]
```

- **serial_device**: e.g. `/dev/ttyS0`, `/dev/ttyUSB0` (use what your board exposes).
- **baud_rate**: optional; default **115200**.
- The standalone build (`build_standalone.sh`) produces **`ne503_mcu_cli_tool`**. If you build the example from the full `hal_v2` tree, the target may still be named `hal-test-peripheral-all-func`; usage is the same.

After startup you get an interactive shell; type `help` for a short command list; `quit` or `exit` stops the program.

## Return lines

Most commands print a line like:

```text
ret=<code> (<text>) dt_ms=<ms>
```

- `ret`: HAL status code; `HAL_OK` means success (see `hal_common.h`).
- `dt_ms`: Request duration in milliseconds (monotonic clock).

Query commands may print extra fields (duty, ADC, booleans, etc.).

---

## LEDs and IR-cut

### `led_id` indices (NE503 / `bsp_led_t`)

Matches MCU firmware `bsp_led_t`. **`led_set`** / **`led_get`** use this **`<id>`**:

| `id` | Role | Hardware notes (MCU) |
|------|------|-------------------------|
| **0** | **IR near** | TIM1 CH1 |
| **1** | **IR far** | TIM1 CH2 |
| **2** | **White 2** (WHITE2) | TIM3 CH2 |
| **3** | **White 1** (WHITE1) | TIM3 CH1, shared PWM with **id 4** |
| **4** | **IR1** | TIM3 CH1, shared with **id 3**; **WLED_EN / RLED_EN** select white vs IR |

**Notes**

- **0 / 1**: Near and far IR fill lights (separate PWM channels).
- **2**: Dedicated white channel (WHITE2).
- **3 / 4**: Share **TIM3 CH1**. Enabling white (3) or IR (4) switches the corresponding enable pins; only one path should be active at a time logically; firmware may mirror duty between the paired indices (see `bsp_ctrl_set_led_duty`).

### Commands

| Command | Arguments | Description |
|---------|-----------|-------------|
| `led_set` | `<id> <0-100>` | Set PWM duty (percent). |
| `led_get` | `<id>` | Read duty (0–100). |
| `ircut_set` | `day` \| `night` | IR-cut filter: `day` / `night` (`HAL_IRCUT_DAY` / `HAL_IRCUT_NIGHT`). |
| `ircut_get` | (none) | Print `day` or `night`. |

---

## MCU and RTC

| Command | Arguments | Description |
|---------|-----------|-------------|
| `mcu_ver` | (none) | Read MCU firmware version. |
| `mcu_ping` | `<u32>` | Send 32-bit value; echoed in `echo=`. |
| `mcu_echo` | `<hexbytes>` | Raw echo test (even-length hex, e.g. `deadbeef`). |
| `rtc_get` | (none) | Read RTC: year (0–99), month, day, weekday, hour, minute, second. |
| `rtc_set` | `<yy> <mm> <dd> <wday> <hh> <min> <sec>` | Set RTC; two-digit year; weekday **1–7** per protocol. |

---

## Sensors (ADC)

| Command | Description |
|---------|-------------|
| `pd_get` | Photo / PD channel: `mv` millivolts; `milli` scaled value (e.g. lux×1000, firmware-defined). |
| `temp_get` | Temperature-related ADC; same fields. |
| `ain_get` | Generic analogue input. |

---

## Environment (fan / heater / radar / reset)

| Command | Arguments | Description |
|---------|-----------|-------------|
| `fan_set` | `<0\|1>` | `1` on, `0` off. |
| `fan_get` | (none) | Query fan enable. |
| `heat_set` | `<0\|1>` | Heater enable. |
| `heat_get` | (none) | Query heater. |
| `radar_set` | `<0\|1>` | Radar power / enable (firmware-defined). |
| `radar_get` | (none) | Query radar. |
| `reset_soc` | (none) | Reset SoC via MCU (`PWR_RST`). |
| `reset_mcu` | `[0\|1]` | Default or **`0`**: MCU reboot command; **`1`**: pulse MCU **NRST** from host GPIO (NE503: HAL GPIO **18**, gpiochip1 line 2). |

---

## Alarms and outputs

| Command | Arguments | Description |
|---------|-----------|-------------|
| `alarm_out_set` | `<ch> <0\|1>` | Alarm output channel **0** or **1**. |
| `alarm_out_get` | `<ch>` | Read alarm output. |
| `wiegand_out_set` | `<ch> <0\|1>` | Wiegand output **0** / **1**. |
| `wiegand_out_get` | `<ch>` | Read Wiegand output. |
| `outputs_get` | (none) | Read all four: `aout0`, `aout1`, `w0`, `w1`. |
| `alarm_sub` | (none) | Subscribe to alarm inputs; prints `[ALARM_IN] ch=… level=…`. |
| `alarm_unsub` | (none) | Unsubscribe. |

---

## RS485

| Command | Arguments | Description |
|---------|-----------|-------------|
| `rs485_init` | `<baud> <cfg3>` | Init baud and framing; **cfg3** is three chars, e.g. **`8N1`**. |
| `rs485_deinit` | (none) | Deinit. |
| `rs485_tx` | `<hexbytes>` | Transmit hex payload. |
| `rs485_sub` | (none) | Subscribe RX; prints `[RS485_RX]`. |
| `rs485_unsub` | (none) | Unsubscribe. |

---

## Lens (low-level HAL)

Call `lens_init` / `lens_deinit` as needed. `lens_cfg`: **0** = all, **1** = iris only, **2** = motor only.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `lens_state` | (none) | Zoom/focus state, positions, RZ done flags. |
| `lens_cfg` | `<0\|1\|2>` | Configuration subset. |
| `lens_zoom_rz` / `lens_focus_rz` | (none) | Zoom / focus reset-to-zero. |
| `lens_zoom_abs` / `lens_focus_abs` | `<pps> <pos>` | Absolute move: `pps` speed, `pos` target. |
| `lens_zoom_run` / `lens_focus_run` | `<pps> <steps>` | Relative move: `steps` steps. |
| `lens_iris_target` | `<0..1023>` | Iris target. |
| `lens_iris_adc` | (none) | Read iris ADC. |
| `lens_sub` / `lens_unsub` | (none) | Lens events → `[LENS_EVT]`. |

---

## Autofocus AF0832 (`hal_lens_af0832`)

Typical flow: `af_create` → optional `af_bootstrap` → motion commands → `af_destroy`.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `af_create` | (none) | Create AF0832 context (default params). |
| `af_destroy` | (none) | Destroy context. |
| `af_bootstrap` | (none) | Calibration / bootstrap (library-defined). |
| `af_zoom_abs` / `af_focus_abs` | `<pps> <pos>` | Absolute zoom/focus. |
| `af_zoom_run` / `af_focus_run` | `<pps> <steps>` | Relative run. |
| `af_goto` | `<zoom_ratio> <distance_m>` | Focus by ratio and distance; **`distance_m ≤ 0`** means infinity; uses spec table (**1 table step ≈ 4 HAL steps**, see source). |
| `af_rz_force` | (none) | Force zoom + focus reset-zero. |

AF events print as `[AF_EVT]` (callback installed on create).

---

## GPIO (Linux gpiod, host)

GPIO numbers are HAL-global indices (NE503 device tree / mapping). This is separate from **`reset_mcu 1`** NRST handling.

| Command | Arguments | Description |
|---------|-----------|-------------|
| `gpio_export` | `<num> <in\|out> <0\|1>` | Export and set direction; third arg: **`0`** normal (logic 1 = pin high); non-**`0`** **ACTIVE_LOW** (logic 1 = pin low). |
| `gpio_unexport` | `<num>` | Release line; call `gpio_unsub` first if subscribed. |
| `gpio_set` | `<num> <0\|1>` | Drive output. |
| `gpio_get` | `<num>` | Read line. |
| `gpio_sub` | `<num> <none\|rising\|falling\|both>` | Edge subscription → `[GPIO_EVT]`. |
| `gpio_unsub` | `<num>` | Unsubscribe. |

---

## Other

| Command | Description |
|---------|-------------|
| `help` | Print built-in help (same commands as this file). |
| `quit` / `exit` | Exit the program. |

Unknown or malformed input prints: `unknown or invalid command, try: help`.

---

## Source references

- CLI implementation: `test_peripheral_all_func.cpp`
- LED indices (MCU): `bsp_led_t` in `mcu_board_prj/.../bsp_ctrl/bsp_ctrl.h`
- Host link protocol: `host_link_proto.h` (keep HAL and MCU firmware in sync)
