# AIPC OS A/B upgrade

The OS upgrade path is intentionally separate from the existing AIPC
application/MCU firmware update.

## Runtime layout

All mutable upgrade data is stored on the persistent data partition:

```text
/data/aipc-os-upgrade/
├── incoming/<job-id>.part
├── packages/<job-id>.swu
├── jobs/<job-id>/status.json
├── jobs/<job-id>/swupdate.log
├── active-job
└── install.lock

/data/backups/aipc-os-upgrade/
├── current -> <job-id>
└── <job-id>/
    ├── manifest.json
    ├── SHA256SUMS
    ├── status
    ├── app-manifest.json
    ├── systemd.tar.gz
    ├── network.tar.gz
    ├── ssh.tar.gz
    └── device-config.tar.gz
```

The API streams multipart uploads to `.part`, calls `fsync`, and atomically
renames the completed package. It requires the package size plus a 2 GiB free
space reserve and at least 512 MiB available memory.

Immediately before either A/B writing or single-copy Recovery staging, the
updater creates a job-specific backup under `/data/backups`. The backup is
written to a temporary directory, checksummed, marked ready, renamed, and only
then selected through the atomic `current` symlink. A backup failure aborts the
upgrade before SWUpdate or the boot-copy selector is touched. The AIPC runtime
tree lives under the canonical `/data/aipc` root on the persistent `/data`
partition (p3), so it survives the rootfs rewrite in place and is no longer
backed up. The backup set covers the independently versioned application
units, device-specific `network`/`ssh`, the `app-manifest`, and a small
system-configuration whitelist:

- time and NTP projection: `/etc/timezone`, `/etc/localtime`,
  `/etc/systemd/timesyncd.conf`, and the AIPC timesyncd provider list;
- static resolver configuration only when `/etc/resolv.conf` is a regular file
  on the old system;
- local CA certificates from `/usr/local/share/ca-certificates`;
- the field calibration file at `/home/root/apps/resources/final_calibration.json`.

`/usr/bin` and `/usr/libexec` symlinks (which live on the rewritten rootfs) are
rebuilt deterministically by `aipc-restore` from `/data/aipc/bin` and
`/data/aipc/libexec`. The boot control units, sysctl, journald, container
runtime and loader configuration always come from the new OS image.

## Services

- `aipc-os-updater.service` writes only the inactive A/B copy. A failed
  `swupdate` never changes the selected boot copy.
- `aipc-os-reboot.service` records the rebooting state before reboot.
- `aipc-os-verify.service` first runs `aipc-os-updater needs-verify`. Ordinary
  boots and stale terminal jobs skip without pulling in any runtime service;
  a pending post-boot verify/rollback job explicitly restarts
  `aipc-autostart`, then checks the selected copy, OS version, restore
  completion, and service health for 60 seconds. A failure selects the
  previous copy and reboots. Application compatibility is advisory here —
  an incompatible app yields terminal `success` with
  `compatibility_valid=false` and skips the health window (see
  [Unconditional OS upgrade](#unconditional-os-upgrade-and-self-healing)).
- `aipc-restore.service` is embedded in the OS and runs before
  `network-pre.target`. It verifies `SHA256SUMS`, restores network/SSH and the
  system-configuration whitelist first, then rebuilds executable links while
  preserving OS-owned definitions.
- `aipc-firstboot.service` runs after restore. Its OS-owned entry point
  `/usr/libexec/aipc-firstboot` execs the full runtime script from the
  canonical `/data/aipc/scripts/aipc-firstboot.sh` root, with a minimal-init
  fallback when `/data/aipc` is empty on a fresh rootfs.

The bootloader must also enforce a finite boot-attempt counter. Userspace
verification cannot recover a target image that never reaches systemd.

## Application compatibility model

OS upgrades and application (AIPC package) upgrades answer compatibility in
opposite directions:

- **OS upgrades are unconditional.** Application compatibility never blocks,
  fails, or rolls back an OS upgrade. An incompatible app is recorded on the
  job (`compatibility_valid=false` + `compatibility_warning`) and surfaced in
  the web UI; adapting the application afterwards is the operator's job.
- **Application upgrades must be compatible** with the running OS before they
  may install.

### Version-range contract

The app manifest (`opt/aipc/app-manifest.json` inside the package, installed
at `/data/aipc/app-manifest.json`) declares a closed OS version range:

```json
{
  "machine": "hailo15-ne503",
  "product": "ne503",
  "min_os_version": "1.12.0",
  "max_os_version": "1.14.0",
  "target_data_schema": 2,
  "supported_data_schema": [1, 2]
}
```

Both bounds must be strict `x.y.z` (three numeric components) and are compared
semantically (`1.9.0 < 1.10.0`). Checks, in order:

| Check | Failure code |
|---|---|
| `/etc/aipc-os-release` exists but has no `MACHINE` or an invalid `OS_VERSION` | `APP_OS_METADATA_UNAVAILABLE` |
| manifest absent from the package | `APP_MANIFEST_MISSING` |
| bounds not `x.y.z`, or `min > max` | `APP_COMPATIBILITY_METADATA_INVALID` |
| `machine` differs from the OS machine | `APP_MACHINE_MISMATCH` |
| `product` differs (compared only when both sides are non-empty) | `APP_PRODUCT_MISMATCH` |
| `min_os_version <= OS_VERSION <= max_os_version` fails | `APP_OS_VERSION_UNSUPPORTED` |
| `supported_data_schema` does not contain the current data schema (falls back to `target_data_schema` when the schema file is absent) | `APP_DATA_SCHEMA_UNSUPPORTED` |

Legacy images with no `/etc/aipc-os-release` at all allow the install with a
warning. Packaging defaults `min_os_version = max_os_version = <current OS
version>` (`AIPC_OS_VERSION ?= 1.12.0` in the Makefile), which is equivalent
to an exact match; widen the range explicitly when a package is known to span
OS versions.

The same rules are enforced at three points — API advisory, API hard gate, and
an on-device backstop that cannot be bypassed through the API:

1. `OTAParseFirmware` (advisory): the parse response carries
   `compatibility: {valid, error_code?, message?, os_version,
   app_min_os_version, app_max_os_version}` so the UI can show *why* a package
   is incompatible before install.
2. `performOTAUpgrade` (hard gate): install is refused before `systemd-run`
   when the verdict is invalid.
3. `scripts/deploy.sh check_package_compatibility` (backstop): re-checks the
   extracted package on the device.
4. `aipc-compat-check` as `ExecStartPre` on every app unit (hard gate): an
   incompatible app can install but its services stay down.

### Minimal mutual exclusion

The two upgrade pipelines may not mutate the system at the same time:

- An OS job in a non-terminal state (including `ready` and `awaiting_reboot`)
  rejects app firmware installs — upload and parse stay available.
- A non-terminal app OTA job rejects OS installs — upload and validate stay
  available.

## Unconditional OS upgrade and self-healing

Application compatibility is advisory at every OS-upgrade decision point:

| Stage | Behaviour |
|---|---|
| `validate` | SWU↔device checks (machine/product/signature/build-time/downgrade) stay hard; the installed app's verdict is recorded as `compatibility_valid` + warning and the job still reaches `ready` |
| install | no application-compatibility check |
| `aipc-restore.service` | advisory — the backup is restored even when it was taken from an app built for another OS range, otherwise the rescue channel would die with it |
| `aipc-os-verify.service` | OS self-checks stay hard (booted copy, OS version, restore `.done` marker, real-failure rollback); the booted-app verdict is advisory — an incompatible app yields terminal `success` with `compatibility_valid=false` and skips the service health window |
| service start | every app unit keeps its hard `aipc-compat-check` `ExecStartPre` except `platform-api`, which runs the check `--warn-only` because it *is* the rescue channel |

Self-healing path after a cross-version OS upgrade:

```text
OS upgrade completes -> reboot -> restore runs (advisory)
  -> incompatible app units held down by their ExecStartPre gate
  -> platform-api starts (warn-only gate) and serves the web UI
  -> UI shows compatibility_valid=false + warning on the OS job
  -> operator installs an app package whose range covers the new OS
  -> gates pass, services start, device is healthy again
```

If no compatible package is available, the previous A/B copy can still be
re-selected (bootloader rollback) — an unconditional OS upgrade never removes
that option.

Accepted costs of this design:

- A single `[min, max]` range cannot exclude a specific bad middle version;
  narrow the range around it instead.
- When the OS moves past `max_os_version`, older app packages stop installing
  and must be repackaged with a raised range.
- Packages that only carry the removed `required_compat_level` field are
  rejected with `APP_COMPATIBILITY_METADATA_INVALID` and must be rebuilt.

## Production configuration

The following environment variables may be set on `platform-api.service`:

```text
AIPC_OS_UPGRADE_DIR=/data/aipc-os-upgrade
AIPC_OS_MACHINE=hailo15-ne503
AIPC_OS_PRODUCT=ne503
AIPC_OS_HARDWARE_VERSION=1.0
AIPC_OS_REQUIRE_SIGNATURE=false
AIPC_OS_REQUIRE_BUILD_TIME=true
AIPC_OS_ALLOW_DOWNGRADE=false
AIPC_COPY_A_VALUE=a
AIPC_COPY_B_VALUE=b
AIPC_FILESYSTEM_DEVICE=mmcblk1
AIPC_RECOVERY_DIR=/data/aipc/recovery
AIPC_BACKUP_ROOT=/data/backups/aipc-os-upgrade
```

The shipped `platform-api.service` enables build-time checks and pins
machine/product/hardware metadata. The SWUpdate **signature gate is relaxed**
for hailo15-ne503 (`AIPC_OS_REQUIRE_SIGNATURE=false`): the on-device SWUpdate
path does not cryptographically verify the image (`run_swupdate.sh` passes `-k`
only for Hailo-10h, and `CONFIG_SIGNED_IMAGES` is compiled in only for
hailo10-usb-dongle), so validation is presence-only for `sw-description.sig`.
Flip the gate back to `true` once CMS signing is wired for the hailo15 image
and a device public key is deployed.

The verifier accepts:

```text
AIPC_PERSISTENT_DATA=/data/aipc-data
AIPC_DATABASE_PROBE=/data/aipc-data/platform.db
```

## Image requirement

The new rootfs must contain the `aipc-bootstrap` package from
`meta-hailo-camthink`. This package provides:

```text
/usr/libexec/aipc-restore
/usr/libexec/aipc-firstboot
/lib/systemd/system/aipc-restore.service
/lib/systemd/system/aipc-firstboot.service
/lib/systemd/system/<aipc-service>.service
/etc/systemd/system.conf.d/<aipc-manager-drop-in>.conf
/etc/systemd/journald.conf.d/<aipc-journal-drop-in>.conf
/etc/sysctl.d/99-aipc-watchdog.conf
/etc/ld.so.conf.d/aipc.conf
/etc/aipc-os-release
```

`packagegroup-camthink-aipc-system` depends on `aipc-bootstrap`, so the normal
NE503 image receives these files automatically. The AIPC release tree (binaries,
libraries, configs, models, applications) lives under the canonical `/data/aipc`
root on persistent `/data`, so it survives the rootfs rewrite in place; only
`/usr/bin` and `/usr/libexec` symlinks are rebuilt by `aipc-restore`. The backup
under `/data/backups` carries application units, network, SSH, the small
system-configuration whitelist, and the app manifest.

Boot ordering is:

```text
mount /data
  -> verify and restore network + SSH + AIPC
  -> start network
  -> run full aipc-firstboot.sh
  -> start event-bus/camera/AI/API/app services
  -> run OS upgrade verification
```

## Required A/B partition layout

Dual-copy online OS upgrade uses:

```text
/dev/mmcblk1p1  copy A boot
/dev/mmcblk1p2  copy A rootfs
/dev/mmcblk1p3  copy B boot
/dev/mmcblk1p4  copy B rootfs
/dev/mmcblk1p5  persistent data
```

Run the read-only check on a device:

```bash
/data/aipc/scripts/aipc-os-layout-check.sh
```

Legacy devices with `p1=boot`, `p2=rootfs`, `p3=data` use
`single-recovery` mode. The updater extracts and validates the Recovery
artifacts from the uploaded OS SWU, caches them under `/data/aipc/recovery`,
backs up the existing files from `p1`, atomically stages the extracted
`fitImage` and recovery rootfs, stores
`swupdate_update_filename=local:/aipc-os-upgrade/packages/<job>.swu`, selects
Hailo's `remote_update` boot image, and waits for explicit reboot. Recovery
mounts `p3`, writes `stable,copy-a`, then returns to copy A.

The uploaded SWU must contain an AIPC-compatible Recovery rootfs. The updater
extracts only the Recovery boot artifacts and skips the main rootfs while
building this on-device cache:

```text
/data/aipc/recovery/
├── manifest.json
├── fitImage
└── swupdate-image-hailo15-ne503.ext4.gz
```

The generated manifest binds the machine, protocol, recovery version, optional
secure-boot key ID, artifact sizes and SHA-256 values. The Recovery rootfs
inside the uploaded SWU must contain `AIPC_LOCAL_RECOVERY_V1`.

Both validation and installation repeat the layout check. A boot-copy/root
mount mismatch or a mounted inactive A/B partition stops the job before any
rootfs write.

### Optional migration to A/B

Legacy devices can continue using recovery upgrades. To gain inactive-copy
writes and rollback, they must be migrated to A/B. They cannot be converted
while running from `p2`, because creating the dual GPT deletes and recreates
the partition table and moves data to `p5`.

Use this offline recovery procedure:

1. Back up all required content from `/data` (legacy `p3`) to another host.
2. Publish the matching `fitImage`, SWUpdate init image, and
   `hailo-update-image-hailo15-ne503.swu` to the recovery/TFTP server.
3. Boot to the U-Boot menu and select **eMMC AB Board Init**. The configured
   update modes are `init-partitions-dual,init-scu-bl,copy-a`.
4. Boot copy A and verify that `p1` through `p5` exist.
5. Restore persistent content to `/data` (`p5`).
6. Run `aipc-os-layout-check.sh`, then test A→B and B→A upgrades.

Do not run `init-partitions-dual` from the installed rootfs.

## Recovery packaging

`pack-release` and `docker-pack-release` no longer require a Yocto SWU during AIPC
package compilation and no longer pre-bundle `/opt/aipc/recovery`. Single-copy
OS upgrade gets its Recovery from the OS SWU uploaded on the device page.

## Device validation

Before production enablement, test:

1. Corrupt CPIO, rootfs gzip, SHA-256, signature, machine and hardware data.
2. Insufficient disk/memory and concurrent requests.
3. Browser disconnect and `platform-api` restart during installation.
4. Power interruption at each SWUpdate phase.
5. A→B, B→A, successful verification, userspace rollback and bootloader
   boot-attempt rollback.
