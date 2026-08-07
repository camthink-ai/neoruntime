#!/usr/bin/env bash
#
# Deploy HAL v2 shared libraries and example binaries (auto-detect: BUILD_DIR/hal-*).
# Libraries: default build is a single libaipc_hal.so*; with -DHAL_V2_BUILD_MONOLITHIC=OFF
# the split libhal-common / libhal-hailo15-* / libhal-devices set is used instead.
# to a target host (Hailo-15), or copy to a local tree.
# Modeled after hal/scripts/deploy_to_hailo.sh (v1).
#
# Remote (default): scp artifacts from the cross-build directory to the target, fix .so symlinks, refresh ldconfig.
#
# Usage:
#   ./hal_v2/scripts/deploy_hal_v2_to_hailo.sh [BUILD_DIR] [TARGET]
#   ./hal_v2/scripts/deploy_hal_v2_to_hailo.sh [BUILD_DIR] --local DEST_DIR
#   ./hal_v2/scripts/deploy_hal_v2_to_hailo.sh --local DEST_DIR   # BUILD_DIR defaults to hal_v2/build-hailo15
#
# Examples (from repo root, after cmake --build hal_v2/build-hailo15):
#   hal_v2/scripts/deploy_hal_v2_to_hailo.sh
#   hal_v2/scripts/deploy_hal_v2_to_hailo.sh hal_v2/build-hailo15 root@10.0.0.1
#   DEPLOY_PASSWORD=root hal_v2/scripts/deploy_hal_v2_to_hailo.sh
#   hal_v2/scripts/deploy_hal_v2_to_hailo.sh hal_v2/build-hailo15 --local ./deploy_staging/opt/aipc
#
# BUILD_DIR: CMake build dir (default: <hal_v2>/build-hailo15).
# TARGET:    ssh destination (default: root@192.0.2.94).
# Auto password: set DEPLOY_PASSWORD (e.g. root); requires sshpass.
#
# On target:
#   libs  -> /opt/aipc/lib/hal_v2   (separate from v1 HAL under /opt/aipc/lib/hal)
#   bins  -> /opt/aipc/bin (includes clip_vit_b_32_embedding_post.json next to hal-ai-example-v2)
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HAL_V2_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
DEFAULT_BUILD="$HAL_V2_DIR/build-hailo15"

LIB_DEST="${INSTALL_PREFIX:-/opt/aipc}/lib/hal_v2"
BIN_DEST="${INSTALL_PREFIX:-/opt/aipc}/bin"
CLIP_EMBEDDING_POST_JSON="$HAL_V2_DIR/examples/ai_example_v2/clip_vit_b_32_embedding_post.json"
EXAMPLES_DEST="/opt/aipc/etc/hal_v2/examples"
VIDEO_TEST_SUB_V2_EXAMPLES_SRC="$HAL_V2_DIR/examples/video_test_sub_v2/data"
VIDEO_TEST_SUB_V2_EXAMPLES_DEST="${EXAMPLES_DEST}/video_test_sub_v2"

# Optional: DEPLOY_PASSWORD + sshpass
if [[ -n "${DEPLOY_PASSWORD:-}" ]] && command -v sshpass >/dev/null 2>&1; then
    run_ssh() { sshpass -p "$DEPLOY_PASSWORD" ssh -o StrictHostKeyChecking=accept-new "$@"; }
    run_scp() { sshpass -p "$DEPLOY_PASSWORD" scp -o StrictHostKeyChecking=accept-new "$@"; }
    echo "Using DEPLOY_PASSWORD with sshpass"
else
    run_ssh() { ssh "$@"; }
    run_scp() { scp "$@"; }
    if [[ -n "${DEPLOY_PASSWORD:-}" ]]; then
        echo "Warning: DEPLOY_PASSWORD set but sshpass not found; ssh/scp will prompt (install: apt install sshpass)" >&2
    fi
fi

# Copy HAL v2 shared libs: monolithic libaipc_hal.so* (default CMake) or legacy split libs.
copy_hal_v2_libs_to() {
    local dest_lib="$1"
    shopt -s nullglob
    local mono=( "${BUILD_DIR}"/libaipc_hal.so* )
    local legacy=(
        "${BUILD_DIR}"/libhal-common.so*
        "${BUILD_DIR}"/libhal-hailo15-*.so*
        "${BUILD_DIR}"/libhal-devices.so*
    )
    if [[ ${#mono[@]} -gt 0 ]]; then
        cp -a "${BUILD_DIR}"/libaipc_hal.so* "${dest_lib}/"
    elif [[ ${#legacy[@]} -gt 0 ]]; then
        cp -a "${BUILD_DIR}"/libhal-common.so* "${dest_lib}/" 2>/dev/null || true
        cp -a "${BUILD_DIR}"/libhal-hailo15-*.so* "${dest_lib}/" 2>/dev/null || true
        cp -a "${BUILD_DIR}"/libhal-devices.so* "${dest_lib}/" 2>/dev/null || true
    else
        echo "Error: no HAL v2 libs in $BUILD_DIR (expect libaipc_hal.so* or libhal-common / libhal-hailo15-* / libhal-devices)" >&2
        exit 1
    fi
}

deploy_local() {
    local dest="$1"
    mkdir -p "${dest}/lib/hal_v2" "${dest}/bin" "${dest}/etc/hal_v2/examples"
    copy_hal_v2_libs_to "${dest}/lib/hal_v2"
    shopt -s nullglob
    local bins=( "${BUILD_DIR}"/hal-* )
    for f in "${bins[@]}"; do
        [[ -f "$f" ]] || continue
        [[ -x "$f" ]] || continue
        cp -a "$f" "${dest}/bin/"
    done
    if [[ -f "$CLIP_EMBEDDING_POST_JSON" ]]; then
        cp -a "$CLIP_EMBEDDING_POST_JSON" "${dest}/bin/"
    fi
    if [[ -d "$VIDEO_TEST_SUB_V2_EXAMPLES_SRC" ]]; then
        mkdir -p "${dest}/etc/hal_v2/examples/video_test_sub_v2"
        cp -a "${VIDEO_TEST_SUB_V2_EXAMPLES_SRC}/"*.json "${dest}/etc/hal_v2/examples/video_test_sub_v2/" 2>/dev/null || true
    else
        echo "Warning: video_test_sub_v2 example JSON dir not found: $VIDEO_TEST_SUB_V2_EXAMPLES_SRC (skipped)" >&2
    fi
    echo "Deployed HAL v2 artifacts to ${dest} (lib/hal_v2, bin, etc/hal_v2/examples)"
}

# Recreate SONAME symlinks (.so -> .so.N -> .so.N.M.P) for versioned HAL libs in a remote dir,
# then assert (a) every symlink resolves to a canonical versioned file and (b) libaipc_hal.so
# byte-matches the host build. CRITICAL: only pure N.M.P versioned files qualify for ln -sf;
# a backup caught by the glob (.bak/.bak2/.backup-*/.af_dbg/.old/.new/.orig) corrupts the SONAME
# and the daemon silently loads a stale ABI — startup SIGILL on 93.48 2026-06-16, and stale-720p
# + SLOW dispatch on 93.72 2026-06-24 (libaipc_hal.so.2 -> libaipc_hal.so.2.0.0.bak2). The prior
# *.bak|*.old|*.new|*.orig blacklist missed .bak2/.backup-*/.af_dbg, so this now uses a positive
# regex plus an md5 check against the exact artifact just uploaded.
fix_hal_symlinks_remote() {
    local dir="$1"
    # Host-side: the canonical versioned file just deployed = newest libaipc_hal.so.N.M.P in
    # BUILD_DIR, and its md5, for the post-deploy assertion that the loader picks up this build.
    local canonical_bn canonical_md5
    canonical_bn=$(cd "$BUILD_DIR" 2>/dev/null && ls -1 libaipc_hal.so.[0-9]*.[0-9]*.[0-9]* 2>/dev/null | sort -V | tail -1 || true)
    if [[ -n "$canonical_bn" && -f "$BUILD_DIR/$canonical_bn" ]]; then
        canonical_md5=$(md5sum "$BUILD_DIR/$canonical_bn" | awk '{print $1}')
    fi

    run_ssh "$TARGET" "bash -s" <<REMOTE
set -e
cd "$dir"

# 1) Recreate SONAME only for pure N.M.P versioned files. Any backup suffix is
#    rejected by the regex, so ln -sf never targets a stale-ABI backup.
for f in libaipc_hal.so.*.*.* libhal-*.so.*.*.*; do
    [[ -f "\$f" ]] || continue
    [[ "\$f" =~ \.so\.[0-9]+\.[0-9]+\.[0-9]+\$ ]] || continue
    base="\${f%.so.*}"
    ver_rest="\${f#\${base}.so.}"
    major="\${ver_rest%%.*}"
    [[ -n "\$major" ]] || continue
    ln -sf "\$f" "\${base}.so.\${major}"
    ln -sf "\${base}.so.\${major}" "\${base}.so"
done

# 2) Positive guard: every HAL symlink must resolve to a canonical versioned file
#    whose basename ends in .so.N.M.P (regex on the final component — NOT a suffix
#    blacklist, which missed .bak2 / .backup-* / .af_dbg on 93.72).
for s in libaipc_hal.so libaipc_hal.so.* libhal-*.so libhal-*.so.*; do
    [[ -L "\$s" ]] || continue
    rt="\$(readlink -f "\$s" 2>/dev/null || true)"
    if [[ -z "\$rt" ]]; then
        echo "ERROR: \$s is a dangling symlink in $dir — HAL SONAME corrupt, aborting deploy" >&2
        exit 1
    fi
    if [[ ! "\$(basename "\$rt")" =~ \.so\.[0-9]+\.[0-9]+\.[0-9]+\$ ]]; then
        echo "ERROR: \$s -> \$rt does not resolve to a canonical versioned .so (N.M.P) in $dir — HAL SONAME corrupt, aborting deploy" >&2
        exit 1
    fi
done
REMOTE

    # 3) md5 assertion: the file the loader actually dlopen()s must byte-match the
    #    canonical host artifact. Catches stale-link, wrong-path, and partial-copy
    #    cases that would otherwise silently load an old ABI and "succeed".
    if [[ -n "$canonical_md5" ]]; then
        run_ssh "$TARGET" "bash -s" <<REMOTE
set -e
cd "$dir"
rt="\$(readlink -f libaipc_hal.so)"
[[ -f "\$rt" ]] || { echo "ERROR: libaipc_hal.so does not resolve to a file in $dir" >&2; exit 1; }
got="\$(md5sum "\$rt" | awk '{print \$1}')"
if [[ "\$got" != "$canonical_md5" ]]; then
    echo "ERROR: libaipc_hal.so -> \$rt md5=\$got, expected $canonical_md5 ($canonical_bn) — deploy did NOT take effect, aborting" >&2
    exit 1
fi
echo "OK: $dir/libaipc_hal.so -> \$rt (md5=$canonical_md5 verified)"
REMOTE
    else
        echo "Warning: could not determine canonical libaipc_hal version/md5 in $BUILD_DIR; skipped md5 assertion for $dir" >&2
    fi
}

deploy_remote() {
    TARGET="$1"
    echo "Build dir:   $BUILD_DIR"
    echo "Target:      $TARGET"
    echo "Remote lib:  $LIB_DEST"
    echo "Remote bin:  $BIN_DEST"
    echo "Remote etc:  $VIDEO_TEST_SUB_V2_EXAMPLES_DEST"

    if [[ ! -d "$BUILD_DIR" ]]; then
        echo "Error: build dir not found: $BUILD_DIR" >&2
        echo "From repo root: cmake -B hal_v2/build-hailo15 ... && cmake --build hal_v2/build-hailo15 -j\$(nproc)" >&2
        exit 1
    fi

    run_ssh "$TARGET" "mkdir -p $LIB_DEST $BIN_DEST $VIDEO_TEST_SUB_V2_EXAMPLES_DEST"

    shopt -s nullglob
    local mono=( "${BUILD_DIR}"/libaipc_hal.so* )
    if [[ ${#mono[@]} -gt 0 ]]; then
        run_scp "${BUILD_DIR}"/libaipc_hal.so* "${TARGET}:${LIB_DEST}/"
    else
        local legacy=( "${BUILD_DIR}"/libhal-common.so* "${BUILD_DIR}"/libhal-hailo15-*.so* "${BUILD_DIR}"/libhal-devices.so* )
        if [[ ${#legacy[@]} -eq 0 ]]; then
            echo "Error: no HAL v2 libs in $BUILD_DIR (expect libaipc_hal.so* or libhal-common / libhal-hailo15-* / libhal-devices)" >&2
            exit 1
        fi
        run_scp "${BUILD_DIR}"/libhal-common.so* "${TARGET}:${LIB_DEST}/" 2>/dev/null || true
        run_scp "${BUILD_DIR}"/libhal-hailo15-*.so* "${TARGET}:${LIB_DEST}/" 2>/dev/null || true
        run_scp "${BUILD_DIR}"/libhal-devices.so* "${TARGET}:${LIB_DEST}/" 2>/dev/null || true
    fi

    fix_hal_symlinks_remote "$LIB_DEST"

    shopt -s nullglob
    local bins=( "${BUILD_DIR}"/hal-* )
    for f in "${bins[@]}"; do
        [[ -f "$f" ]] || continue
        [[ -x "$f" ]] || continue
        local bn
        bn="$(basename "$f")"
        run_scp "$f" "${TARGET}:${BIN_DEST}/"
        run_ssh "$TARGET" "chmod +x ${BIN_DEST}/${bn}"
    done

    if [[ -f "$CLIP_EMBEDDING_POST_JSON" ]]; then
        run_scp "$CLIP_EMBEDDING_POST_JSON" "${TARGET}:${BIN_DEST}/"
    else
        echo "Warning: CLIP post JSON not in tree: $CLIP_EMBEDDING_POST_JSON (skipped)" >&2
    fi

    # Copy example JSON for hal-video-test-sub-v2
    if [[ -d "$VIDEO_TEST_SUB_V2_EXAMPLES_SRC" ]]; then
        run_scp "${VIDEO_TEST_SUB_V2_EXAMPLES_SRC}/"*.json "${TARGET}:${VIDEO_TEST_SUB_V2_EXAMPLES_DEST}/" 2>/dev/null || true
    else
        echo "Warning: video_test_sub_v2 example JSON dir not found: $VIDEO_TEST_SUB_V2_EXAMPLES_SRC (skipped)" >&2
    fi

    echo "Configuring dynamic linker for HAL v2..."
    run_ssh "$TARGET" "mkdir -p /etc/ld.so.conf.d && echo '$LIB_DEST' > /etc/ld.so.conf.d/aipc-hal-v2.conf && ldconfig"

    # Also copy monolithic HAL to v1 path (/opt/aipc/lib/hal/) since camera-daemon
    # loads from there via rpath=$ORIGIN/../lib/hal.
    local V1_LIB="/opt/aipc/lib/hal"
    shopt -s nullglob
    local v1_mono=( "${BUILD_DIR}"/libaipc_hal.so* )
    if [[ ${#v1_mono[@]} -gt 0 ]]; then
        echo "Syncing monolithic HAL to v1 path ${V1_LIB} (for camera-daemon rpath)..."
        run_ssh "$TARGET" "mkdir -p ${V1_LIB}"
        run_scp "${BUILD_DIR}"/libaipc_hal.so* "${TARGET}:${V1_LIB}/"
        run_ssh "$TARGET" "rm -f ${V1_LIB}/libaipc_hal.so.*.new"
        fix_hal_symlinks_remote "$V1_LIB"
        run_ssh "$TARGET" "ldconfig"
    fi

    if run_ssh "$TARGET" "ldconfig -p 2>/dev/null | grep -q hal_v2"; then
        echo "OK: HAL v2 libs visible to loader"
    else
        echo "Warning: verify on target: cat /etc/ld.so.conf.d/aipc-hal-v2.conf && ldconfig -p | grep hal_v2" >&2
    fi

    echo "Done."
    echo "  Bins: ${BIN_DEST}/hal-* (auto-copied from BUILD_DIR)"
    echo "  Example JSON: ${VIDEO_TEST_SUB_V2_EXAMPLES_DEST}/frontend_v4l2_multires_3streams.json"
}

# --- main (argument order matches v1: BUILD_DIR then TARGET; use --local for filesystem copy) ---
if [[ "${1:-}" == "--local" ]]; then
    if [[ -z "${2:-}" ]]; then
        echo "Usage: $0 --local DEST_DIR   or   $0 [BUILD_DIR] --local DEST_DIR" >&2
        exit 1
    fi
    BUILD_DIR="$DEFAULT_BUILD"
    echo "Local copy mode (BUILD_DIR=$BUILD_DIR) -> ${2}"
    deploy_local "${2}"
    exit 0
fi

if [[ "${2:-}" == "--local" ]]; then
    if [[ -z "${3:-}" ]]; then
        echo "Usage: $0 [BUILD_DIR] --local DEST_DIR" >&2
        exit 1
    fi
    BUILD_DIR="${1:-$DEFAULT_BUILD}"
    echo "Local copy mode (BUILD_DIR=$BUILD_DIR) -> ${3}"
    deploy_local "${3}"
    exit 0
fi

BUILD_DIR="${1:-$DEFAULT_BUILD}"
TARGET="${2:-root@192.0.2.94}"
deploy_remote "$TARGET"
