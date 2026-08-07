# ne503_boot_prep

Boot-time helper: talks to the MCU over UART. Optionally syncs **UTC** with the MCU RTC (host from MCU or MCU from host), optionally checks firmware version, and runs **OTA** when appropriate (same flow as `ota_test_v2`: `OTA_ENTER_BOOT` plus bootloader Ymodem-CRC).

## Build

- **Standalone** (same idea as `test_peripheral_all_func`, no `libhal.so` at runtime): run `./build_standalone.sh` in this directory. Requires **libgpiod** (from the cross sysroot when cross-building; on the host install `libgpiod-dev`).
- **Integrated in hal_v2**: with `HAL_PLATFORM=hailo15`, CMake builds **`ne503_boot_prep`** with the same linking pattern as `hal-ota-test-v2`.

### Cross-compilation (Yocto SDK)

Do **not** rely on `OEToolchainConfig.cmake` alone without loading the SDK environment; CMake may still pick the host `/usr/bin/gcc` and mix it with the ARM sysroot, producing errors such as `bits/timesize-32.h: No such file or directory`.

Recommended steps:

1. `source <SDK>/environment-setup-armv8a-poky-linux` (adjust path to your SDK).
2. In the **same shell**, run `./build_standalone.sh` (the script passes `CROSS_COMPILE` / `OECORE_TARGET_SYSROOT` into CMake so the cross toolchain is used, e.g. `aarch64-poky-linux-gcc`).
3. If you previously configured without sourcing the environment, delete the build tree and reconfigure: `rm -rf build-standalone`.

## Required arguments

`--rtc` and `--version-check` are **independent** and can both be set (e.g. RTC sync/push first, then version check and OTA if needed). Order of execution: RTC step → version logic → OTA.

| Argument | Values |
|----------|--------|
| `--rtc` | `none`: skip RTC; `sync-from-mcu`: read MCU RTC and write **UTC** to Linux `CLOCK_REALTIME` (usually requires **root** / `CAP_SYS_TIME`); `push-to-mcu`: write current Linux UTC time to the MCU RTC. |
| `--version-check` | `off`: do not read version or run OTA; `on`: must supply `--firmware` and (depending on skips) `--expect-version`. |

## Common defaults (optional to omit)

| Argument | Default |
|----------|---------|
| `--serial` | `/dev/ttyS0` |
| `--baud` | `921600` |
| `--timeout-ms` | `1500` (host_link request timeout) |
| `--mcu-reset-gpio` | `18` (NE503 MCU NRST; see `HalMcuConfig` comments) |

## Extra arguments when `--version-check on`

- **`--firmware`**: Required. Path to the OTA package (magic: `HOST_LINK_OTA_PACKAGE_MAGIC`).
- **`--expect-version <x.y.z|x.y.z.w>`**: Required when package or MCU comparison is needed; may be omitted only if both **`--skip-package-version-check`** and **`--skip-mcu-version-check`** are set.
  - **Three components** `x.y.z`: treated as `x.y.z.0` for ordering against the MCU; for **match / skip OTA** under `mismatch`, only major.minor.patch are compared (build ignored).
  - **Four components** `x.y.z.w`: compare major, minor, patch, build against the MCU.
- By default the header field `app_version` is compared to `--expect-version` **semantically** (`0.1.2` equals `0.1.2.0`). If your pack script only writes three segments, use e.g. `--expect-version 0.1.2`.
- **`--skip-package-version-check`**: Do not compare header `app_version` to `--expect-version` (magic and readable header still checked). Use when the packaged version string does not match your CLI naming but you trust the image path.
- **`--skip-mcu-version-check`**: Do not call `get_version`; after firmware checks (unless package check is skipped), **run OTA directly**. Incompatible with **`--upgrade-if`** (a warning is printed and `--upgrade-if` is ignored). If **`--skip-package-version-check`** is also set, **`--expect-version`** may be omitted (only **`--firmware`** is needed).
- **`--upgrade-if mismatch|older`** (default **`mismatch`**):
  - **`mismatch`**: If `get_version` succeeds and the MCU already meets the target (three-part match on maj/min/patch, or full quad match), **skip OTA**. If reading version fails, still attempt OTA.
  - **`older`**: OTA only when the MCU version is **strictly less** than the target (lexicographic quad; `x.y.z` target uses `x.y.z.0`). If already at or above target, skip OTA. If reading version fails, still attempt OTA.

## OTA behaviour summary

- When an upgrade is needed, runs `HAL_OTA_OPS.install_from_file` (conditions depend on `--upgrade-if`).
- Each attempt uses **`ymodem_crc_char_timeout_ms = 5000`** (wait for bootloader Ymodem-CRC **`C`**).
- Up to **3** attempts; if an attempt returns **`HAL_ERR_TIMEOUT`** (typically no `C` within 5 s), the next attempt is preceded by **`reset_mcu(..., true)`** on **`--mcu-reset-gpio`** and **`--settle-after-reset-ms`** wait (defaults align with internal **2000 ms** settle).
- Inside `install_from_file`, failed `OTA_ENTER_BOOT` may trigger soft reset / NRST / SoC reset per `HalOtaInstallOptions` (`force_reboot_on_enter_boot_fail = true`).

## Optional tuning

- `--post-boot-delay-ms`: Delay after entering bootloader before Ymodem (similar to HAL examples, e.g. 800 ms).
- `--settle-after-reset-ms`: Wait after outer-layer **`HAL_ERR_TIMEOUT`** NRST pulse before retry.
- `--mcu-reset-active-low`, `--mcu-reset-pulse-ms`: Same meaning as in `ota_test_v2`.

## Exit codes

| Code | Meaning |
|------|---------|
| 0 | Success (including skipped OTA) |
| 1 | Invalid arguments |
| 2 | `HAL_MCU_OPS.init` failed |
| 3 | RTC step failed |
| 4 | Firmware header validation or read failed |
| 5 | OTA failed after 3 attempts |

## Notes

- **Baud rate** must match the running MCU firmware; otherwise host_link may not respond and failed version reads will still lead into OTA, which may then fail.
- **`sync-from-mcu`** fails without permission to set the system clock (see `errno` in the log).
