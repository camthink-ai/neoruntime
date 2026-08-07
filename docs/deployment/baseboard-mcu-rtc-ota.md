# Baseboard MCU — RTC sync & firmware OTA

The NE503 baseboard is an STM32-class MCU that owns the lens (zoom / focus /
iris), IR-cut, fan, heater, LED, RS485, GPIO, analog I/O, alarms, and a real
time clock. The host talks to it over an **exclusive-use** UART (`/dev/ttyS0`
@ 921600) using the `host_link` protocol. This document describes how the host
synchronizes the MCU RTC and performs field OTA of the MCU firmware at boot,
gated so it never breaks platform bring-up.

The mechanism is three small pieces:

| Piece | Path | Purpose |
|-------|------|---------|
| `ne503_boot_prep` | `/data/aipc/bin/ne503_boot_prep` | Cross-built aarch64 tool that does the RTC step + version check + OTA over `host_link` (see `/home/share/USAGE.md`). |
| `aipc-mcu-prep.sh` | `/data/aipc/bin/aipc-mcu-prep.sh` | Boot wrapper: picks sane flags, calls the binary, maps exit codes, **always exits 0**. |
| `aipc-mcu-prep.service` | `/etc/systemd/system/aipc-mcu-prep.service` | Boot oneshot, ordered in the right window, enable-only (never started mid-deploy). |

The unit sets `HOME=/run/aipc/mcu-prep` and creates `.hailo` there before
launching the helper. This gives the linked Hailo logging/runtime code a
writable per-boot home without creating state under `/root`.

The unit and wrapper share a per-boot marker at
`/run/aipc/mcu-prep/attempted`. The wrapper writes it after confirming
`ne503_boot_prep` exists, and the unit has a matching negative
`ConditionPathExists`. If another boot transaction, current-root rebuild, or
manual start asks for `aipc-mcu-prep.service` again in the same boot, the
duplicate start is skipped without touching `/dev/ttyS0`. The marker is tmpfs
state, so the next reboot gets a fresh attempt.

The packaged journald drop-in `zz-aipc-console.conf` overrides the BSP's
`MaxLevelConsole=err` with `MaxLevelConsole=info`. `ForwardToConsole` remains
disabled globally; only units that explicitly select `journal+console`, such
as `aipc-mcu-prep.service`, emit their info-level progress on the serial
console.

MCU firmware packages live under `/data/aipc/firmware/mcu/` and are named
`ne503_ota_package_v<X.Y.Z>.bin` (e.g. `ne503_ota_package_v0.1.5.bin`). The
factory flashing package `ne503_Main_v*.hex` is **not** used in the field OTA
path — it is for SWD/STLink factory programming only.

---

## Boot ordering (the UART exclusivity constraint)

`/dev/ttyS0` is exclusive-use. At runtime it is owned by `camera-daemon`
(and `device-control` for lens / PTZ / light). The MCU prep tool is a separate
process and must run **before** any of those daemons open the UART, and exit
before they claim it.

The boot graph is:

```
aipc-firstboot.service          (hwclock --hctosys, sysctl/core_pattern/journal)
        │ After=
        ▼
aipc-mcu-prep.service           (this unit — RTC push + OTA, oneshot)
        │ Before=
        ▼
aipc-autostart.service          (enable + systemctl start --no-block of…)
        │
        ▼
camera-daemon, device-control, ai-runtime, …   (runtime owners of /dev/ttyS0)
```

`aipc-autostart.service` is the single chokepoint that queues every runtime
daemon, so ordering `aipc-mcu-prep` **before** it (and additionally before
`camera-daemon`/`device-control` as belt-and-suspenders) guarantees the UART is
free. The unit is `enable`-only in `deploy.sh` — it is never `start`ed
mid-deploy, because that would race the running `camera-daemon` for the UART.
If the OS or current-root launcher nevertheless queues it twice in one boot,
the per-boot marker converts the later start into a no-op.

> RTC step runs first inside `ne503_boot_prep`, then the version check / OTA.
> The two are independent and always run in that order in a single invocation.

---

## RTC synchronization — host → MCU only

Direction is **push-to-mcu**: the host writes its current UTC time into the MCU
RTC. This direction was chosen because the host already has a battery-backed
I²C RTC (HYM8563 / PCF85363 / RV3028, probed by `aipc-firstboot.sh`) plus NTP,
so its clock is the authoritative one; the MCU RTC is the side that drifts.

The reverse direction (`sync-from-mcu`) is **not used** — it would be dead
weight that also requires `CAP_SYS_TIME` and can fight `hwclock`.

Plausibility guard: the wrapper reads `date +%Y` and only pushes when
`year >= 2024`. If the battery RTC is empty and NTP has not yet converged
(year reports as the build epoch or 1970), it passes `--rtc none` for that boot
and tries again next boot. This prevents writing a bogus time into the MCU.

---

## Firmware OTA

Each boot, if at least one package is staged under `/data/aipc/firmware/mcu/`:

1. The wrapper picks the highest version by filename (`ls -1v … | tail -n 1`).
2. The version is parsed from the filename (`ne503_ota_package_v0.1.5.bin` →
   `0.1.5`) and passed as `--expect-version`.
3. `ne503_boot_prep` validates the package (magic + header `app_version`
   semantic match), compares against the MCU's running version, and flashes
   only when `--upgrade-if older` is satisfied.

`--upgrade-if older` means: flash iff the MCU is **strictly older** than the
package. Consequences:

- Normal boot with a current MCU → fast no-op (no re-flash, no UART bus hold
  beyond a quick `get_version`).
- MCU older than the staged package → upgrade.
- MCU newer than the staged package → **no downgrade**.

If no package is staged, the wrapper passes `--version-check off` and only the
RTC step runs.

### `ne503_boot_prep` exit codes

| Code | Meaning | Wrapper behavior |
|------|---------|------------------|
| 0 | success / skip | log `rtc=ok ota=done` |
| 1 | param error (wrapper bug) | `ERROR`, points at the wrapper |
| 2 | `HAL_MCU_OPS.init` fail — MCU not responding or `/dev/ttyS0` busy | `WARN`, skipped this boot |
| 3 | RTC step failed | `WARN`, retries next boot |
| 4 | firmware header check failed (bad / mismatched package) | `WARN`, names the package |
| 5 | OTA failed after 3 retries (NRST pulse + settle between tries) | `ERROR`, MCU unchanged, retries next boot |

The wrapper maps every non-zero code to a log line and **always exits 0**, so a
missing MCU or a failed OTA never blocks platform boot.

`aipc-mcu-prep.sh` also wraps the binary with a command-level timeout
(`AIPC_MCU_PREP_TIMEOUT_SEC`, default `115`) that is intentionally shorter than
the unit's `TimeoutStartSec=120`. If the lower-level HAL path hangs after a UART
timeout, the wrapper kills the child, logs `rc=124` or `rc=137`, and still exits
0 so systemd does not mark the boot oneshot failed.

---

## Independence from `aipc-cli system disable`

`aipc-cli system disable` only stops/disables the six runtime daemons in
`aipcServices` (`event-bus`, `app-manager`, `ai-runtime`, `camera-daemon`,
`device-control`, `platform-api`). `aipc-mcu-prep` is **not** in that list, so
disabling the platform leaves RTC sync and MCU OTA running at every boot. This
is intentional: MCU firmware currency and clock sanity are baseboard concerns,
not platform-runtime concerns.

---

## Build & staging

`ne503_boot_prep` is cross-built for aarch64 out of the HAL tree (source:
`hal_v2/examples/ne503_boot_prep/ne503_boot_prep.cpp`):

```bash
# integrated with the HAL build (recommended; same SDK/sysroot as the device HAL,
# lands in hal_v2/build-hailo15/):
make hal-v2 HAL_PLATFORM=hailo15

# standalone (cross-build; source the Poky SDK environment first, needs libgpiod):
./hal_v2/examples/ne503_boot_prep/build_standalone.sh
#   -> output: hal_v2/examples/ne503_boot_prep/build-standalone/ne503_boot_prep
```

> Use the integrated build for release. `tools/ne503_boot_prep`, if present, is a
> pre-placed copy of unknown provenance (often a debug build) and must NOT be
> used as the release baseline — an SDK/SONAME mismatch can SIGILL on the device
> (same class of bug as the ai-runtime BSP skew). The Makefile stages it only as
> a last resort and logs `provenance unverified`.

`pack-release` rebuilds and syncs MCU firmware by default before release
staging:

```bash
make pack-release VERSION=...
```

This runs `make -C mcu_board_prj RELEASE=1`, copies delivery artifacts from
`mcu_board_prj/build/` into `mcu_board_prj/firmware/`, and then performs the
normal release staging. Override the MCU build flags with `MCU_MAKE_ARGS=...`
when needed.

To package already-built MCU artifacts without rebuilding them:

```bash
make pack-release BUILD_MCU_FW=0 VERSION=...
```

The prep binary is searched, in order:

1. `hal_v2/build-$(HAL_PLATFORM)/ne503_boot_prep` — `make hal-v2` integrated output.
2. `hal_v2/examples/ne503_boot_prep/build-standalone/ne503_boot_prep` — `build_standalone.sh` output.
3. `tools/ne503_boot_prep` — pre-placed copy (provenance unverified; logged).

The OTA package is globbed first from the MCU project's release-artifact
directory `mcu_board_prj/firmware/ne503_ota_package_*.bin`, then from the
compatibility input directory `firmware/mcu/ne503_ota_package_*.bin`. Packages
from both locations are staged under `opt/aipc/firmware/mcu/`. Absence is a
clean skip, not a build failure — the unit's
`ConditionPathExists=/data/aipc/bin/ne503_boot_prep` turns a bare/unprovisioned
image into a no-op at boot.

With the default MCU build enabled, no manual copy is needed before packing.
An externally supplied package can still be added through the compatibility
directory when using `BUILD_MCU_FW=0`:

```bash
mkdir -p firmware/mcu
cp /home/share/ne503_ota_package_v0.1.5.bin firmware/mcu/
make pack-release BUILD_MCU_FW=0 VERSION=...
```

Out-of-band deploy is also fine — copy the package directly to
`/data/aipc/firmware/mcu/` on the device and reboot.

---

## Deploy & enable

`scripts/deploy.sh`:

- stages the three artifacts into the release root (via Makefile rules above),
- installs `systemd/aipc-mcu-prep.service` through the generic
  `systemd/*.service` loop,
- **enables** `aipc-mcu-prep.service` in the boot-unit enable loop (alongside
  `aipc-firstboot` / `aipc-autostart`), and never starts it.

No OS image rebuild is required to add this unit — `deploy.sh` installs systemd
units at runtime under `/etc/systemd/system/`, and the Yocto factory image
path picks up any `aipc-*.service` via glob. See the project memory entry on
the `/data/aipc` canonical install root for the broader install topology.

---

## Verification (bench setup)

```bash
# 1. after deploy + reboot, the unit ran and ordered before autostart:
systemctl status aipc-mcu-prep.service
journalctl -b -u aipc-mcu-prep.service --no-pager | grep 'complete'

# expect: [aipc-mcu-prep] .. complete (rtc=ok ota=done rc=0)

# 2. ordering: mcu-prep finished before camera-daemon started
journalctl -b | grep -E 'aipc-mcu-prep.*(starting|complete)|Started Camera Daemon'

# 3. a fresh package triggers exactly one upgrade, then a no-op next boot:
#    drop a newer ne503_ota_package_v<X.Y.Z>.bin into /data/aipc/firmware/mcu/
#    and reboot twice; first boot logs ota=done, second boot is a fast skip.

# 4. aipc-cli system disable does NOT suppress it:
aipc-cli system disable && reboot
journalctl -b -u aipc-mcu-prep.service --no-pager | grep complete   # still runs
aipc-cli system enable   # restore
```

### Expected console output (boot)

```
[aipc-mcu-prep] 09:14:02 baseboard MCU boot prep starting
[aipc-mcu-prep] 09:14:02 INFO RTC: push host UTC to MCU (year=2026)
[aipc-mcu-prep] 09:14:02 INFO OTA: package ne503_ota_package_v0.1.5.bin expect=0.1.5 upgrade-if=older (no downgrade, no re-flash)
[aipc-mcu-prep] 09:14:02 INFO running (timeout=115s): /data/aipc/bin/ne503_boot_prep --serial /dev/ttyS0 --baud 921600 --rtc push-to-mcu --version-check on --firmware /data/aipc/firmware/mcu/ne503_ota_package_v0.1.5.bin --expect-version 0.1.5 --upgrade-if older
…
[aipc-mcu-prep] 09:14:04 INFO boot_prep succeeded (rc=0)
[aipc-mcu-prep] 09:14:04 complete (rtc=ok ota=done rc=0)
```

### Troubleshooting

| Symptom | Likely cause | Check |
|---------|--------------|-------|
| `rtc=skip ota=skip`, "ne503_boot_prep not found" | bare image or binary not staged | confirm `/data/aipc/bin/ne503_boot_prep` exists; redeploy |
| `duplicate=skip` | the service was queued more than once in the same boot | check current-root/app-bootstrap ordering; the duplicate did not access UART |
| `rc=2` | MCU offline, or another daemon grabbed `/dev/ttyS0` first | `systemctl status camera-daemon`; verify the unit's `Before=` ordering held |
| `rc=4` | package magic / version mismatch | filename version vs. header `app_version`; replace the package |
| `rc=5` | OTA failed 3× (bootloader Ymodem) | cabling / power / bootloader integrity; retries next boot |
| `rc=124` or `rc=137` | `ne503_boot_prep` exceeded the wrapper timeout | inspect HAL UART/deinit path; wrapper kept boot moving and retries next boot |
| RTC not advancing | host clock not sane at prep time | `date` in `aipc-firstboot`; check battery RTC / NTP |
