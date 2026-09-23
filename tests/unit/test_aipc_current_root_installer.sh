#!/bin/bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
INSTALLER="$ROOT/scripts/aipc-install-current-root.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

DATA="$TMP/data/aipc"
ROOTFS="$TMP/rootfs"
mkdir -p \
    "$DATA/bin" \
    "$DATA/libexec" \
    "$DATA/lib/hal" \
    "$DATA/scripts" \
    "$DATA/systemd" \
    "$DATA/etc/systemd/system.conf.d" \
    "$DATA/etc/systemd/journald.conf.d" \
    "$DATA/etc/sysctl.d" \
    "$DATA/etc/security" \
    "$ROOTFS/etc"

cp "$ROOT/scripts/aipc-compat-check.sh" "$DATA/libexec/aipc-compat-check"
chmod 0755 "$DATA/libexec/aipc-compat-check"
cat >"$DATA/app-manifest.json" <<'EOF'
{
  "app_version": "test",
  "machine": "hailo15-ne503",
  "product": "ne503",
  "min_os_version": "1.12.0",
  "max_os_version": "1.12.0",
  "supported_data_schema": [1],
  "target_data_schema": 1
}
EOF
cat >"$ROOTFS/etc/aipc-os-release" <<'EOF'
OS_VERSION=1.12.0
MACHINE=hailo15-ne503
PRODUCT=ne503
EOF
cp "$ROOTFS/etc/aipc-os-release" "$TMP/os-release.before"
printf 'test\n' >"$DATA/VERSION"
printf '#!/bin/sh\nexit 0\n' >"$DATA/bin/platform-api"
printf '#!/bin/sh\nexit 0\n' >"$DATA/bin/camera-daemon"
printf '#!/bin/sh\nexit 0\n' >"$DATA/libexec/aipc-restore"
printf '#!/bin/sh\nexit 0\n' >"$DATA/libexec/aipc-os-updater"
printf '#!/bin/sh\nexit 0\n' >"$DATA/scripts/aipc-configure-platform-api-gateway.py"
printf '#!/bin/sh\nexit 0\n' >"$DATA/scripts/aipc-install-current-root.sh"
printf '#!/bin/sh\nexit 0\n' >"$DATA/scripts/aipc-firstboot.sh"
chmod 0755 "$DATA/bin/platform-api" "$DATA/bin/camera-daemon" \
    "$DATA/libexec/aipc-restore" "$DATA/libexec/aipc-os-updater" \
    "$DATA/scripts/aipc-configure-platform-api-gateway.py" \
    "$DATA/scripts/aipc-install-current-root.sh" "$DATA/scripts/aipc-firstboot.sh"
printf 'listen: 8443\n' >"$DATA/etc/platform-api.yaml"
printf 'driver: test\n' >"$DATA/etc/camera-daemon.yaml"
printf '[Unit]\nDescription=Test\n' >"$DATA/systemd/aipc-platform.target"
printf '[Manager]\nRuntimeWatchdogSec=20s\n' >"$DATA/etc/systemd/system.conf.d/watchdog.conf"
printf '[Journal]\nStorage=persistent\n' >"$DATA/etc/systemd/journald.conf.d/persist.conf"
printf 'kernel.panic = 10\n' >"$DATA/etc/sysctl.d/panic.conf"
printf '{}\n' >"$DATA/etc/security/seccomp-default.json"

AIPC_INSTALL_ROOT="$DATA" AIPC_ROOTFS_PREFIX="$ROOTFS" "$INSTALLER"

cmp "$TMP/os-release.before" "$ROOTFS/etc/aipc-os-release"
grep -qx 'OS_VERSION=1.12.0' "$ROOTFS/etc/aipc-os-release"
grep -qx 'MACHINE=hailo15-ne503' "$ROOTFS/etc/aipc-os-release"
grep -qx 'PRODUCT=ne503' "$ROOTFS/etc/aipc-os-release"
[[ "$(readlink "$ROOTFS/usr/bin/platform-api")" == "$DATA/bin/platform-api" ]]
[[ "$(readlink "$ROOTFS/usr/bin/camera-daemon")" == "$DATA/bin/camera-daemon" ]]
[[ "$(readlink "$ROOTFS/usr/libexec/aipc-restore")" == "$DATA/libexec/aipc-restore" ]]
[[ "$(readlink "$ROOTFS/usr/libexec/aipc-os-updater")" == "$DATA/libexec/aipc-os-updater" ]]
cmp "$DATA/systemd/aipc-platform.target" "$ROOTFS/etc/systemd/system/aipc-platform.target"
cmp "$DATA/etc/systemd/system.conf.d/watchdog.conf" "$ROOTFS/etc/systemd/system.conf.d/watchdog.conf"
cmp "$DATA/etc/systemd/journald.conf.d/persist.conf" "$ROOTFS/etc/systemd/journald.conf.d/persist.conf"
cmp "$DATA/etc/sysctl.d/panic.conf" "$ROOTFS/etc/sysctl.d/panic.conf"
cmp "$DATA/etc/security/seccomp-default.json" "$ROOTFS/etc/aipc/seccomp-default.json"
grep -qx "$DATA/lib/hal" "$ROOTFS/etc/ld.so.conf.d/aipc.conf"

# The OS bootstrap invokes the current-root installer on every boot. Existing
# disabled units must stay disabled, and the stable platform target must not be
# started because it Wants the entire runtime regardless of enable state.
for name in aipc-firstboot.service aipc-autostart.service platform-api.service aipc-logrotate.timer; do
    printf '[Unit]\nDescription=Test %s\n[Install]\nWantedBy=multi-user.target\n' "$name" >"$DATA/systemd/$name"
done
mkdir -p "$ROOTFS/etc/systemd/system"
cp "$DATA/systemd/aipc-firstboot.service" "$ROOTFS/etc/systemd/system/aipc-firstboot.service"
cp "$DATA/systemd/aipc-autostart.service" "$ROOTFS/etc/systemd/system/aipc-autostart.service"
cp "$DATA/systemd/platform-api.service" "$ROOTFS/etc/systemd/system/platform-api.service"

FAKE_BIN="$TMP/fake-bin"
SYSTEMCTL_LOG="$TMP/systemctl.log"
mkdir -p "$FAKE_BIN"
cat >"$FAKE_BIN/systemctl" <<'EOF'
#!/bin/sh
printf '%s\n' "$*" >>"$SYSTEMCTL_LOG"
if [ "$1" = "is-enabled" ]; then
    case "$2" in
        aipc-autostart.service|platform-api.service)
            printf 'disabled\n'
            exit 1
            ;;
        aipc-platform.target)
            printf 'static\n'
            exit 0
            ;;
        *)
            printf 'enabled\n'
            exit 0
            ;;
    esac
fi
exit 0
EOF
chmod 0755 "$FAKE_BIN/systemctl"

SYSTEMCTL_LOG="$SYSTEMCTL_LOG" \
AIPC_INSTALL_ROOT="$DATA" \
AIPC_ROOTFS_PREFIX="$ROOTFS" \
AIPC_TEST_ACTIVATE_WITH_ROOTFS_PREFIX=1 \
AIPC_SYSTEMCTL="$FAKE_BIN/systemctl" \
AIPC_SYSCTL=/bin/true \
AIPC_LDCONFIG=/bin/true \
    "$INSTALLER" >"$TMP/activate.out"

if grep -Eq '^enable (aipc-autostart|platform-api)\.service$' "$SYSTEMCTL_LOG"; then
    echo "installer re-enabled an explicitly disabled unit" >&2
    cat "$SYSTEMCTL_LOG" >&2
    exit 1
fi
grep -qx 'enable aipc-logrotate.timer' "$SYSTEMCTL_LOG" || {
    echo "installer did not enable a newly installed unit" >&2
    cat "$SYSTEMCTL_LOG" >&2
    exit 1
}
start_line="$(grep '^start --no-block ' "$SYSTEMCTL_LOG" || true)"
if [[ "$start_line" == *aipc-autostart.service* || "$start_line" == *aipc-platform.target* ]]; then
    echo "installer bypassed the persisted runtime disable state" >&2
    cat "$SYSTEMCTL_LOG" >&2
    exit 1
fi
grep -q 'not starting aipc-autostart.service (preserved disabled state)' "$TMP/activate.out" || {
    echo "installer did not report the preserved autostart state" >&2
    cat "$TMP/activate.out" >&2
    exit 1
}

# A failed rootfs copy must propagate out of the installer. This specifically
# guards the errexit contract used by the generic OS launcher.
mkdir "$TMP/fail-bin"
cat >"$TMP/fail-bin/cp" <<'EOF'
#!/bin/sh
exit 42
EOF
chmod 0755 "$TMP/fail-bin/cp"
if PATH="$TMP/fail-bin:$PATH" AIPC_INSTALL_ROOT="$DATA" AIPC_ROOTFS_PREFIX="$ROOTFS" \
    "$INSTALLER" >/dev/null 2>&1; then
    echo "installer swallowed a rootfs copy failure" >&2
    exit 1
fi

# Missing required release content must be fatal; the launcher must never
# report success after a partial current-root rebuild.
mv "$DATA/libexec/aipc-compat-check" "$DATA/libexec/aipc-compat-check.missing"
if AIPC_INSTALL_ROOT="$DATA" AIPC_ROOTFS_PREFIX="$ROOTFS" "$INSTALLER" >/dev/null 2>&1; then
    echo "installer unexpectedly accepted a missing compatibility checker" >&2
    exit 1
fi
mv "$DATA/libexec/aipc-compat-check.missing" "$DATA/libexec/aipc-compat-check"

# Legacy manifests that only carry required_compat_level (no min/max OS
# version range) must be rejected: the version-range metadata is mandatory.
cat >"$DATA/app-manifest.json" <<'EOF'
{
  "app_version": "test",
  "machine": "hailo15-ne503",
  "product": "ne503",
  "required_compat_level": 1,
  "supported_data_schema": [1],
  "target_data_schema": 1
}
EOF
if AIPC_INSTALL_ROOT="$DATA" AIPC_ROOTFS_PREFIX="$ROOTFS" \
    "$INSTALLER" >"$TMP/legacy.out" 2>"$TMP/legacy.err"; then
    echo "installer unexpectedly accepted a legacy compat-level manifest" >&2
    exit 1
fi
grep -q 'persistent app manifest has incomplete compatibility metadata' "$TMP/legacy.err" || {
    echo "legacy manifest rejection message mismatch:" >&2
    cat "$TMP/legacy.err" >&2
    exit 1
}

echo "test_aipc_current_root_installer: OK"
