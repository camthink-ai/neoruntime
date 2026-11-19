#!/bin/bash
# Behavioral test for the linknet trust chain in scripts/download_models.sh.
#
# Covers exactly the two failure classes the PR review surfaced:
#   1. a file already sitting at the DPM path must be revalidated, not
#      accepted on existence (a device that ran the old downloader may
#      carry the zoo build there);
#   2. every failure path must propagate: FAIL increments and a non-zero
#      exit, never a silent "successful" run with the person channel
#      disarmed (typo'd LINKNET_SRC, wrong-build source, no source).
#
# The factory digest is env-overridable, so the test pins a fixture digest
# instead of shipping the 1.9MB model. All sources are local paths — the
# URL branch of LINKNET_SRC needs network and stays a bench test.
set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
DL="$ROOT/scripts/download_models.sh"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# --- fixtures ---------------------------------------------------------------
# A "factory" linknet: valid HEF magic + unique payload. The script's
# LINKNET_MD5 is pinned to this fixture's digest via the environment.
FACTORY="$TMP/factory_linknet.hef"
{ printf '\x01HEF\x00\x00\x00\x03'; head -c 4096 /dev/urandom; } >"$FACTORY"
FACTORY_MD5="$(md5sum "$FACTORY" | cut -d' ' -f1)"
# A wrong-but-valid-HEF build: same magic, different content.
WRONG="$TMP/wrong_build.hef"
{ printf '\x01HEF\x00\x00\x00\x03'; head -c 4096 /dev/urandom; } >"$WRONG"

# Fake siblings for the other three core files: the script only SKIP-checks
# them on existence, and the json is generated, so empty HEF-magic stubs do.
seed_core_siblings() {
    local dest="$1"
    mkdir -p "$dest/detection"
    printf '\x01HEF\x00stub' >"$dest/detection/hailo_yolov8n_384_640.hef"
    printf '\x01HEF\x00stub' >"$dest/detection/tiny_yolov4_license_plates.hef"
}

# run <exit-code-assertion> <dest> [env...]
# Executes the downloader in --core mode with LINKNET_MD5 pinned to the
# fixture and asserts BOTH the exit code and the FAIL counter, because the
# review finding was precisely that some paths exit 0 while failing.
run_core() {
    local want_rc="$1" dest="$2"; shift 2
    local out rc
    out="$(env LINKNET_MD5="$FACTORY_MD5" "$@" bash "$DL" --core --dest "$dest" 2>&1)"
    rc=$?
    RUN_OUT="$out"
    if [[ "$rc" != "$want_rc" ]]; then
        echo "FAIL: expected exit $want_rc, got $rc" >&2
        echo "$out" | tail -20 >&2
        exit 1
    fi
    RUN_FAILS="$(grep -c 'FAIL' <<<"$out" || true)"
}

PASS=0
ok() { echo "ok $1 - $2"; PASS=$((PASS + 1)); }

# --- scenarios ---------------------------------------------------------------

# 1. Clean destination, no trusted source: linknet FAIL, non-zero exit.
D="$TMP/s1"; seed_core_siblings "$D"
run_core 1 "$D"
[[ -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef" ]] || true
[[ ! -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef" ]] || { echo "FAIL: s1 file must not exist"; exit 1; }
ok 1 "clean + no source exits 1 with FAIL"

# 2. Zoo-build file already at the path (ran the old downloader): must be
#    quarantined, not trusted, and the run still fails (no source given).
D="$TMP/s2"; seed_core_siblings "$D"; mkdir -p "$D/segmentation"
cp "$WRONG" "$D/segmentation/linknet_mbv1_ss_dpm_256.hef"
run_core 1 "$D"
[[ -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef.unverified-bak" ]] || { echo "FAIL: s2 no quarantine"; exit 1; }
[[ ! -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef" ]] || { echo "FAIL: s2 bad file still in place"; exit 1; }
grep -q "md5 mismatch" <<<"$RUN_OUT" || { echo "FAIL: s2 mismatch not reported"; exit 1; }
ok 2 "existing wrong build is quarantined and reported"

# 3. Zoo-build at the path + operator factory source: quarantine, restore
#    the factory build byte-for-byte, exit 0.
D="$TMP/s3"; seed_core_siblings "$D"; mkdir -p "$D/segmentation"
cp "$WRONG" "$D/segmentation/linknet_mbv1_ss_dpm_256.hef"
run_core 0 "$D" LINKNET_SRC="$FACTORY"
[[ "$(md5sum "$D/segmentation/linknet_mbv1_ss_dpm_256.hef" | cut -d' ' -f1)" == "$FACTORY_MD5" ]] \
    || { echo "FAIL: s3 factory build not restored"; exit 1; }
[[ -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef.unverified-bak" ]] \
    || { echo "FAIL: s3 quarantine missing"; exit 1; }
ok 3 "factory source restores the verified build"

# 4. Typo'd LINKNET_SRC: must FAIL and exit non-zero (the old `|| true`
#    swallowed this and automation deployed with the channel disarmed).
D="$TMP/s4"; seed_core_siblings "$D"
run_core 1 "$D" LINKNET_SRC="$TMP/typo/path.hef"
grep -q "local source not found" <<<"$RUN_OUT" || { echo "FAIL: s4 typo not reported"; exit 1; }
ok 4 "typo'd source path fails loudly"

# 5. Source that is a valid HEF but the wrong build: rejected, removed,
#    non-zero exit, nothing left at the DPM path.
D="$TMP/s5"; seed_core_siblings "$D"
run_core 1 "$D" LINKNET_SRC="$WRONG"
[[ ! -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef" ]] || { echo "FAIL: s5 wrong build left in place"; exit 1; }
grep -q "did not pass factory-build verification" <<<"$RUN_OUT" || { echo "FAIL: s5 rejection not reported"; exit 1; }
ok 5 "wrong-build source is rejected and cleaned"

# 6. Factory build already in place (correct previous run): verified SKIP,
#    exit 0 — the trust check must not reject the genuine artifact.
D="$TMP/s6"; seed_core_siblings "$D"; mkdir -p "$D/segmentation"
cp "$FACTORY" "$D/segmentation/linknet_mbv1_ss_dpm_256.hef"
run_core 0 "$D"
grep -q "SKIP (exists, factory build verified)" <<<"$RUN_OUT" || { echo "FAIL: s6 verified skip missing"; exit 1; }
[[ ! -f "$D/segmentation/linknet_mbv1_ss_dpm_256.hef.unverified-bak" ]] || { echo "FAIL: s6 quarantined a good file"; exit 1; }
ok 6 "genuine factory file is SKIP-verified, not quarantined"

echo
echo "1..6"
[[ "$PASS" == "6" ]] && exit 0
exit 1
