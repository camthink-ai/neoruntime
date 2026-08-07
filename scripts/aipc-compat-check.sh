#!/bin/bash
# Validate that the installed AIPC application can run on the current OS.

set -euo pipefail

OS_COMPAT_FILE="${AIPC_OS_COMPATIBILITY_FILE:-/etc/aipc-os-release}"
APP_MANIFEST_FILE="${AIPC_APP_MANIFEST:-/data/aipc/app-manifest.json}"
DATA_SCHEMA_FILE="${AIPC_DATA_SCHEMA_FILE:-/data/aipc-data/schema-version}"
MAINTENANCE_MARKER="${AIPC_MAINTENANCE_MARKER:-/run/aipc-maintenance-mode}"

fail() {
    echo "[aipc-compat-check] ERROR: $*" >&2
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

if [[ -s "$MAINTENANCE_MARKER" ]]; then
    fail "AIPC_MAINTENANCE_MODE: $(head -1 "$MAINTENANCE_MARKER")"
fi

# Legacy images did not provide compatibility metadata. Keep one migration
# path available; once the OS embeds /etc/aipc-os-release all checks are strict.
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
[[ -r "$DATA_SCHEMA_FILE" ]] || fail "APP_DATA_SCHEMA_MISSING: $DATA_SCHEMA_FILE"

os_machine="$(sed -n 's/^MACHINE=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
os_product="$(sed -n 's/^PRODUCT=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
os_level="$(sed -n 's/^AIPC_COMPAT_LEVEL=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
os_schema="$(sed -n 's/^DATA_SCHEMA=//p' "$OS_COMPAT_FILE" | tr -d "\"'" | head -1)"
app_machine="$(json_string "$APP_MANIFEST_FILE" machine)"
app_product="$(json_string "$APP_MANIFEST_FILE" product)"
app_level="$(json_number "$APP_MANIFEST_FILE" required_compat_level)"
app_schema="$(json_number "$APP_MANIFEST_FILE" target_data_schema)"
current_schema="$(tr -d '[:space:]' <"$DATA_SCHEMA_FILE")"

[[ -n "$os_machine" && -n "$os_level" && -n "$os_schema" ]] ||
    fail "OS_COMPATIBILITY_METADATA_INVALID"
[[ "$os_machine" == "$app_machine" ]] ||
    fail "APP_MACHINE_MISMATCH: OS=$os_machine App=$app_machine"
if [[ -n "$os_product" && -n "$app_product" && "$os_product" != "$app_product" ]]; then
    fail "APP_PRODUCT_MISMATCH: OS=$os_product App=$app_product"
fi
[[ "$os_level" == "$app_level" ]] ||
    fail "APP_COMPAT_LEVEL_MISMATCH: OS=$os_level App=$app_level"
[[ "$os_schema" == "$current_schema" && "$os_schema" == "$app_schema" ]] ||
    fail "APP_DATA_SCHEMA_UNSUPPORTED: OS=$os_schema current=$current_schema App=$app_schema"
schema_supported "$APP_MANIFEST_FILE" "$current_schema" ||
    fail "APP_DATA_SCHEMA_UNSUPPORTED: App does not support schema $current_schema"

echo "[aipc-compat-check] compatible: machine=$os_machine level=$os_level schema=$current_schema"
