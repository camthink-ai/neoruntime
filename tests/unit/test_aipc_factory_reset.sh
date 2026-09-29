#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
RESET="$ROOT/scripts/aipc-factory-reset.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# The reset script hard-requires root; skip (not fail) where that is unavailable.
if [[ ${EUID} -ne 0 ]]; then
    echo "test_aipc_factory_reset: SKIP (requires root)"
    exit 0
fi

# AIPC_ROOT inside $TMP differs from /data/aipc, so the script runs in TEST
# MODE: no systemctl calls, no /etc/systemd/network rewrites on this host.
DATA="$TMP/root"
PAYLOAD="$TMP/payload"
mkdir -p \
    "$DATA/data" \
    "$DATA/apps/instances/app-a" \
    "$DATA/apps/registry" \
    "$DATA/data/event-bus" \
    "$DATA/data/media-backup" \
    "$DATA/network" \
    "$DATA/etc" \
    "$DATA/bin" \
    "$DATA/web" \
    "$DATA/models" \
    "$DATA/log" \
    "$PAYLOAD/etc"

printf '1.0.0\n' >"$DATA/VERSION"
printf 'fake-db\n' >"$DATA/data/platform.db"
printf 'wal\n' >"$DATA/data/platform.db-wal"
printf 'shm\n' >"$DATA/data/platform.db-shm"
printf 'state\n' >"$DATA/apps/instances/app-a/state.json"
printf 'registry\n' >"$DATA/apps/registry/apps.json"
printf 'event\n' >"$DATA/data/event-bus/0001"
printf 'media\n' >"$DATA/data/media-backup/clip.mp4"
printf '[Match]\nName=eth0\n\n[Network]\nAddress=192.168.100.50/24\n' \
    >"$DATA/network/10-eth0.network"
printf 'listen: 8443\n' >"$DATA/etc/platform-api.yaml"
printf 'binary\n' >"$DATA/bin/platform-api"
printf '<html/>\n' >"$DATA/web/index.html"
printf 'model\n' >"$DATA/models/person_v1.hef"
printf 'boot log\n' >"$DATA/log/boot.log"
printf 'factory defaults\n' >"$PAYLOAD/etc/zero.yaml"

# --- dry-run must not change anything --------------------------------------
AIPC_ROOT="$DATA" AIPC_RESET_PAYLOAD_DIR="$PAYLOAD" "$RESET" --yes --dry-run \
    >"$TMP/dry.out" 2>&1

[[ -f "$DATA/data/platform.db" ]]
[[ -f "$DATA/apps/instances/app-a/state.json" ]]
[[ -f "$DATA/network/10-eth0.network" ]]
[[ ! -f "$DATA/etc/zero.yaml" ]]
grep -q 'DRY-RUN' "$TMP/dry.out"

# --- real run (TEST MODE: services/network of this host untouched) ----------
AIPC_ROOT="$DATA" AIPC_RESET_PAYLOAD_DIR="$PAYLOAD" "$RESET" --yes \
    >"$TMP/run.out" 2>&1

# Whitelisted state must be gone.
[[ ! -e "$DATA/data/platform.db" ]]
[[ ! -e "$DATA/data/platform.db-wal" ]]
[[ ! -e "$DATA/data/platform.db-shm" ]]
[[ ! -e "$DATA/apps/instances" ]]
[[ ! -e "$DATA/apps/registry" ]]
[[ ! -e "$DATA/data/event-bus" ]]
[[ ! -e "$DATA/data/media-backup" ]]
[[ ! -e "$DATA/network/10-eth0.network" ]]

# Product content, identity marker and logs must survive.
[[ -f "$DATA/VERSION" ]]
[[ -f "$DATA/bin/platform-api" ]]
[[ -f "$DATA/web/index.html" ]]
[[ -f "$DATA/models/person_v1.hef" ]]
[[ -f "$DATA/log/boot.log" ]]

# Packaged defaults restored into etc/, existing etc/ content kept.
[[ -f "$DATA/etc/zero.yaml" ]]
cmp -s "$PAYLOAD/etc/zero.yaml" "$DATA/etc/zero.yaml"
[[ -f "$DATA/etc/platform-api.yaml" ]]

# TEST MODE must keep systemd and the host network config out of reach.
grep -q 'TEST MODE: skip systemctl stop' "$TMP/run.out"
grep -q 'TEST MODE: skip rewriting host' "$TMP/run.out"
if grep -q '^wrote /etc/systemd/network' "$TMP/run.out"; then
    echo "reset script touched the host network config in TEST MODE" >&2
    exit 1
fi

# --- guards -----------------------------------------------------------------

# A root that lacks the VERSION marker must be refused outright.
GUARD="$TMP/not-an-install"
mkdir -p "$GUARD/data"
if AIPC_ROOT="$GUARD" "$RESET" --yes >"$TMP/guard.out" 2>&1; then
    echo "reset script accepted a non-install root" >&2
    exit 1
fi
grep -q 'not an AIPC install root' "$TMP/guard.out"

# Unknown options must be rejected before anything is touched.
if AIPC_ROOT="$DATA" "$RESET" --yes --explode >"$TMP/opt.out" 2>&1; then
    echo "reset script accepted an unknown option" >&2
    exit 1
fi

echo "test_aipc_factory_reset: OK"
