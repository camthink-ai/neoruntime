#!/bin/bash
# Validate that the installed AIPC application can run on the current OS.
#
# The app manifest declares a closed [min_os_version, max_os_version] range;
# the check passes when the running OS_VERSION falls inside it. Services whose
# start must never be blocked (the rescue channel) invoke this with
# --warn-only: an incompatible verdict is logged but exits 0, so platform-api
# stays reachable and the operator can install a compatible app package.

set -euo pipefail

OS_COMPAT_FILE="${AIPC_OS_COMPATIBILITY_FILE:-/etc/aipc-os-release}"
APP_MANIFEST_FILE="${AIPC_APP_MANIFEST:-/data/aipc/app-manifest.json}"
DATA_SCHEMA_FILE="${AIPC_DATA_SCHEMA_FILE:-/data/aipc-data/schema-version}"
MAINTENANCE_MARKER="${AIPC_MAINTENANCE_MARKER:-/run/aipc-maintenance-mode}"

WARN_ONLY=0
if [[ "${1:-}" == "--warn-only" ]]; then
    WARN_ONLY=1
elif [[ -n "${1:-}" ]]; then
    echo "[aipc-compat-check] ERROR: unknown argument: $1" >&2
    exit 1
fi

fail() {
    echo "[aipc-compat-check] ERROR: $*" >&2
    if (( WARN_ONLY )); then
        echo "[aipc-compat-check] WARN: --warn-only active; continuing so the rescue channel stays up" >&2
        exit 0
    fi
    exit 1
}

json_string() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p' "$1" | head -1
}

json_number() {
    sed -n 's/.*"'"$2"'"[[:space:]]*:[[:space:]]*\([0-9][0-9]*\).*/\1/p' "$1" | head -1
}

schema_supported() {
    local manifest="$1" schema="$2" value values
    values="$(
        tr '\n' ' ' < "$manifest" |
            sed -n 's/.*"supported_data_schema"[[:space:]]*:[[:space:]]*\[\([^]]*\)\].*/\1/p' |
            tr ',' ' '
    )"
    for value in $values; do
        value="${value//[[:space:]]/}"
        [[ "$value" == "$schema" ]] && return 0
    done
    return 1
}

version_valid() {
    [[ "$1" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]
}

# version_lte A B — numeric x.y.z comparison. Hand-rolled because busybox
# `sort -V` availability is not guaranteed across image configurations.
version_lte() {
    local a=(${1//./ }) b=(${2//./ }) i
    for i in 0 1 2; do
        (( 10#${a[i]} < 10#${b[i]} )) && return 0
        (( 10#${a[i]} > 10#${b[i]} )) && return 1
    done
    return 0
}

if [[ -s "$MAINTENANCE_MARKER" ]]; then
    # Deliberate operator hold; never overridden by --warn-only.
    echo "[aipc-compat-check] ERROR: AIPC_MAINTENANCE_MODE: $(head -1 "$MAINTENANCE_MARKER")" >&2
    exit 1
fi

# Legacy images did not provide OS metadata. Keep one migration path
# available; once the OS embeds /etc/aipc-os-release all checks are strict.
if [[ ! -f "$OS_COMPAT_FILE" ]]; then
    echo "[aipc-compat-check] WARN: $OS_COMPAT_FILE is absent; legacy OS compatibility check skipped" >&2
    exit 0
fi

if [[ ! -r "$APP_MANIFEST_FILE" && "$APP_MANIFEST_FILE" == "/data/aipc/app-manifest.json" ]]; then
    for candidate in /data/app-manifest.json /data/aipc/app-manifest.json; do
        if [[ -r "$candidate" ]]; then
            APP_MANIFEST_FILE="$candidate"
            break
        fi
    done
fi

[[ -r "$APP_MANIFEST_FILE" ]] || fail "APP_MANIFEST_MISSING: $APP_MANIFEST_FILE"

os_machine="$(sed -n 's/^MACHINE=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
os_product="$(sed -n 's/^PRODUCT=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
os_version="$(sed -n 's/^OS_VERSION=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
app_machine="$(json_string "$APP_MANIFEST_FILE" machine)"
app_product="$(json_string "$APP_MANIFEST_FILE" product)"
app_min="$(json_string "$APP_MANIFEST_FILE" min_os_version)"
app_max="$(json_string "$APP_MANIFEST_FILE" max_os_version)"
app_schema="$(json_number "$APP_MANIFEST_FILE" target_data_schema)"

[[ -n "$os_machine" ]] ||
    fail "APP_OS_METADATA_UNAVAILABLE: $OS_COMPAT_FILE has no MACHINE"
version_valid "$os_version" ||
    fail "APP_OS_METADATA_UNAVAILABLE: invalid OS_VERSION=$os_version"

[[ -n "$app_machine" && -n "$app_min" && -n "$app_max" && -n "$app_schema" ]] ||
    fail "APP_COMPATIBILITY_METADATA_INVALID: manifest lacks machine/min_os_version/max_os_version (old-format packages must be repackaged)"
version_valid "$app_min" && version_valid "$app_max" ||
    fail "APP_COMPATIBILITY_METADATA_INVALID: min_os_version=$app_min max_os_version=$app_max must be x.y.z"
version_lte "$app_min" "$app_max" ||
    fail "APP_COMPATIBILITY_METADATA_INVALID: min_os_version $app_min is greater than max_os_version $app_max"

[[ "$os_machine" == "$app_machine" ]] ||
    fail "APP_MACHINE_MISMATCH: OS=$os_machine App=$app_machine"
if [[ -n "$os_product" && -n "$app_product" && "$os_product" != "$app_product" ]]; then
    fail "APP_PRODUCT_MISMATCH: OS=$os_product App=$app_product"
fi
version_lte "$app_min" "$os_version" && version_lte "$os_version" "$app_max" ||
    fail "APP_OS_VERSION_UNSUPPORTED: current OS $os_version is outside the app range $app_min-$app_max"

current_schema="$(tr -d '[:space:]' <"$DATA_SCHEMA_FILE" 2>/dev/null || true)"
# Fresh device without a persisted schema: judge against the app's target.
[[ -n "$current_schema" ]] || current_schema="$app_schema"
schema_supported "$APP_MANIFEST_FILE" "$current_schema" ||
    fail "APP_DATA_SCHEMA_UNSUPPORTED: App does not support schema $current_schema"

echo "[aipc-compat-check] compatible: machine=$os_machine os=$os_version range=$app_min-$app_max schema=$current_schema"
