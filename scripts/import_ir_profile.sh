#!/usr/bin/env bash
# Import a retuned IR profile from a tuning-tool export into the two
# repo-managed imaging trees, then run the build-time consistency check.
#
# The IR profile for each lens version lives in its own directory:
#   af0832 -> profiles/IR/AF_IR          (entry "Infrared_Basic")
#   fg2009 -> profiles/IR/gst_example_IR (entry "Infrared_Basic_FG2009")
# Updating one lens never touches the other.
#
# The tuning tool's deploy output (e.g. /etc/imaging/cfg/NE503/.../AF_IR_Profile
# on the board) carries tool-private files and possibly drifted
# encoder/sensor configs; per repo policy only iq_settings.json is imported
# by default. Use --all to copy every file the destination dir already has
# (explicit full sync).
#
# Usage:
#   scripts/import_ir_profile.sh <af0832|fg2009> <src_dir> [--all]
#
#   src_dir: local directory containing the retuned iq_settings.json, e.g.
#     scp -r board:/etc/imaging/cfg/NE503/hailo15h/imx678/theia_sl410m/4k/\
# profiles/Custom/AF_IR_Profile /tmp/ && scripts/import_ir_profile.sh af0832 /tmp/AF_IR_Profile
#
# After importing, rebuild camera-daemon: the bench pipeline serves the IR
# profile from the compiled-in medialib bundle, so a rebuild is required for
# the change to take effect on the board.
set -euo pipefail

die() { echo "import_ir_profile: ERROR: $*" >&2; exit 1; }
info() { echo "import_ir_profile: $*"; }

[[ $# -ge 2 ]] || die "usage: $0 <af0832|fg2009> <src_dir> [--all]"
LENS="$1"; SRC="$2"; MODE="${3:-}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

case "$LENS" in
    af0832) DEST_REL="cfg/hailo15h/imx678/theia_sl410m/4k/profiles/IR/AF_IR" ;;
    fg2009) DEST_REL="cfg/hailo15h/imx678/theia_sl410m/4k/profiles/IR/gst_example_IR" ;;
    *) die "unknown lens '$LENS' (expected af0832|fg2009)" ;;
esac

TREES=(
    "$REPO_ROOT/configs/imaging"
    "$REPO_ROOT/hal_v2/platforms/hailo15/media/default_medialib_overlay/etc/imaging"
)
IQ="$SRC/iq_settings.json"
[[ -f "$IQ" ]] || die "no iq_settings.json in $SRC"
python3 -c "import json,sys; json.load(open(sys.argv[1]))" "$IQ" \
    || die "iq_settings.json is not valid JSON"
for tree in "${TREES[@]}"; do
    [[ -f "$tree/$DEST_REL/iq_settings.json" ]] || die "destination dir missing: $tree/$DEST_REL"
done

# The consistency check needs the SDK sysroot (vendor profiles resolve only
# there). Resolve it BEFORE touching either tree so a missing SDK cannot turn
# a successful import into a failure. Uses the repo's HAILO_SDK_PATH
# convention (Makefile/pack_release.sh), with SDK_PATH and the bench default
# as fallbacks.
SDK_CANDIDATES=("${HAILO_SDK_PATH:-}" "${SDK_PATH:-}" "/opt/hailo-sdk" "$HOME/Desktop/hailo-sdk-4.0.23")
SYSROOT=""
for c in "${SDK_CANDIDATES[@]}"; do
    [[ -n "$c" && -d "$c/sysroots/armv8a-poky-linux" ]] && { SYSROOT="$c/sysroots/armv8a-poky-linux"; break; }
done
[[ -n "$SYSROOT" ]] || die "SDK sysroot not found (set HAILO_SDK_PATH); the post-import consistency check cannot run"

for tree in "${TREES[@]}"; do
    dest="$tree/$DEST_REL"
    if [[ "$MODE" == "--all" ]]; then
        n=0
        for f in "$dest"/*; do
            base="$(basename "$f")"
            [[ -f "$SRC/$base" ]] || continue
            cp "$SRC/$base" "$dest/$base"; n=$((n + 1))
        done
        [[ "$n" -gt 0 ]] || die "full sync copied 0 files into $dest (source has no matching files?)"
        info "full sync: $n files -> $dest"
    else
        cp "$IQ" "$dest/iq_settings.json"
        info "iq_settings.json -> $dest"
    fi
done

# Same verification the build runs, so a bad import fails here, not in CI.
# --overlay takes the overlay root (the check joins etc/imaging/... itself).
CHECK="$REPO_ROOT/hal_v2/platforms/hailo15/media/check_medialib_overlay.py"
OVERLAY_ROOT="$REPO_ROOT/hal_v2/platforms/hailo15/media/default_medialib_overlay"
python3 "$CHECK" --overlay "$OVERLAY_ROOT" --repo-configs "${TREES[0]}" --sysroot "$SYSROOT"
info "done ($LENS, mode=${MODE:-iq-only}); rebuild camera-daemon to refresh the compiled-in bundle"
