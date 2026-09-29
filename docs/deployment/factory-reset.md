# Factory reset

Production-line step: run **after functional tests, before release**. Also
usable when recommissioning or returning a device to the default state. The
reset clears everything an operator or commissioning may have configured and
leaves the platform in its just-deployed state.

## What is reset (strict whitelist, nothing else is touched)

| Path | Content |
|---|---|
| `/data/aipc/data/platform.db*` | platform DB (apps, instances, settings, web login) — re-seeded by `platform-api` on next start |
| `/data/aipc/apps/instances`, `/data/aipc/apps/registry` | app runtime state |
| `/data/aipc/data/event-bus` | persisted events |
| `/data/aipc/data/media-backup` | media backup cache |
| `/data/aipc/network/*.network` | commissioned per-device IP |
| `/etc/systemd/network/10-eth0.network` | rewritten to the image default `10.0.0.1/24` |

## What is preserved

- `/data/aipc` binaries, web assets and models (product content, not config)
- factory identity: SN / MAC live in the U-Boot environment and MCU EEPROM,
  outside `/data/aipc` entirely
- logs under `/data/aipc/log` (kept for RMA traceability)

## Usage

On the device, either directly:

```bash
/data/aipc/scripts/aipc-factory-reset.sh            # interactive
/data/aipc/scripts/aipc-factory-reset.sh --dry-run  # plan only, change nothing
/data/aipc/scripts/aipc-factory-reset.sh --yes      # no prompt (scripts/CI)
```

or through the CLI:

```bash
aipc-cli system factory-reset            # prompts via the script
aipc-cli system factory-reset --dry-run
aipc-cli system factory-reset --yes
```

Options:

- `--yes` — skip the interactive confirmation
- `--dry-run` — print the planned actions, change nothing
- `AIPC_RESET_PAYLOAD_DIR=/path/to/release/opt/aipc` (script only) — also
  restore `/data/aipc/etc/*.yaml` service defaults from the packaged release

The script ships inside the AIPC package at
`opt/aipc/scripts/aipc-factory-reset.sh` (installed at
`/data/aipc/scripts/aipc-factory-reset.sh`).

## After the reset

The device answers at its default address **10.0.0.1/24** on eth0. If your
terminal is connected at the commissioned IP, reconnect there. `platform-api`
re-creates and re-seeds `platform.db` on startup, so the web login returns to
factory defaults.

## Safety model

- Must run as root.
- Refuses any `AIPC_ROOT` that lacks the deployed `VERSION` marker, so a
  typoed root cannot be "reset".
- With `AIPC_ROOT` set to anything other than `/data/aipc` the script enters
  TEST MODE: it still applies the whitelist to the given root, but never
  stops/starts systemd units and never rewrites the host's
  `/etc/systemd/network` — safe to exercise on a dev box or a mounted data
  partition image.
