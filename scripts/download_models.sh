#!/bin/bash
# download_models.sh — Download built-in HEF models from the Hailo model zoo
#
# Usage:
#   bash download_models.sh               # Full preload set (skip genai by default)
#   bash download_models.sh --with-genai  # Also fetch Qwen3-VL-2B (~3GB)
#   bash download_models.sh --core        # Only the 4 AI-privacy-mask (DPM) critical
#                                         # files — the minimum set a production
#                                         # device must carry (ISP denoise HEFs ship
#                                         # with the firmware medialib, not here)
#   bash download_models.sh --dest DIR    # Target dir (default: /data/aipc-data/models
#                                         # on devices, <repo>/models elsewhere)
#
# Works with either curl or wget — production images ship busybox wget only,
# so every download goes through fetch(), which adapts to whichever is present.
#
# Every downloaded file is verified: non-empty AND starts with the HEF magic
# (\x01HEF) so an HTML error page or a truncated transfer can never be left
# behind as a .hef (a bogus file at a DPM path disarms that mask channel
# silently — the 2026-08 field incident).

set -uo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_ROOT="$(dirname "$SELF_DIR")"

# Parse arguments
WITH_GENAI=false
CORE_ONLY=false
DEST_ARG=""
shift_next=0
for arg in "$@"; do
    case "$arg" in
        --with-genai) WITH_GENAI=true ;;
        --skip-genai) WITH_GENAI=false ;; # legacy no-op (genai is opt-in now)
        --core) CORE_ONLY=true ;;
        --dest) shift_next=1 ;;
        *)
            if [[ "$shift_next" == "1" ]]; then
                DEST_ARG="$arg"; shift_next=0
            fi
            ;;
    esac
done

# Default destination: the device model store that camera-daemon DPM hardcodes
# (/data/aipc-data/models — same storage as /data/aipc/models, verified by
# inode on a running device). Fall back to the repo tree for off-device runs.
if [[ -n "$DEST_ARG" ]]; then
    MODEL_BASE="$DEST_ARG"
elif [[ -n "${INSTALL_PREFIX:-}" ]]; then
    MODEL_BASE="$INSTALL_PREFIX/models"
elif [[ -d /data/aipc-data/models || -d /data/aipc-data ]]; then
    MODEL_BASE="/data/aipc-data/models"
else
    MODEL_BASE="$SCRIPT_ROOT/models"
fi

S3_BASE="https://hailo-model-zoo.s3.eu-west-2.amazonaws.com"
GENAI_BASE="https://dev-public.hailo.ai/v5.3.0/blob"
LOCAL_LINKNET="/home/root/apps/dynamic_privacy_mask/resources/linknet_mbv1_ss_dpm_256.hef"

# Tool detection: prefer curl when it actually runs (a broken stub must not
# win), otherwise fall back to (busybox) wget.
CURL=""
if command -v curl >/dev/null 2>&1 && curl --version >/dev/null 2>&1; then
    CURL="$(command -v curl)"
elif ! command -v wget >/dev/null 2>&1; then
    echo "ERROR: neither curl nor wget is available." >&2
    exit 2
fi

OK=0
FAIL=0
SKIP=0

inc() { eval "$1=\$((\$$1 + 1))"; }

# verify_hef <file>: non-empty and HEF magic.
verify_hef() {
    local f="$1"
    [[ -s "$f" ]] || return 1
    LC_ALL=C head -c 4 "$f" | grep -q $'^\x01HEF'
}

# verify_linknet <file>: HEF magic AND the factory preload build digest.
# The public zoo v5.2.0 build is a different compile that loads fine but
# silently breaks the person channel, so for the DPM-critical linknet a
# magic check alone proves nothing — pin the factory build. Update this
# digest together with the factory preload manifest when the model is
# ever re-released.
# Env-overridable so the unit test can pin a fixture digest.
LINKNET_MD5="${LINKNET_MD5:-a8384d45f5fe057673f0e76a2402133a}"
verify_linknet() {
    local f="$1"
    verify_hef "$f" || return 1
    [[ "$(md5sum "$f" 2>/dev/null | cut -d' ' -f1)" == "$LINKNET_MD5" ]]
}

# fetch <dest> <url>: transport-agnostic single download with stall abort.
fetch() {
    local dest="$1" url="$2"
    if [[ -n "$CURL" ]]; then
        "$CURL" --fail --location --silent --show-error \
            --speed-time 60 --speed-limit 1024 \
            --output "$dest" "$url"
    else
        # -T is the (busybox-compatible) network read timeout: aborts a
        # stalled transfer instead of hanging forever; total time unlimited.
        wget -T 60 -q -O "$dest" "$url"
    fi
}

download() {
    local dest="$1"
    local url="$2"

    if [[ -f "$dest" ]]; then
        echo "  SKIP (exists)"
        inc SKIP
        return 0
    fi

    echo "  Downloading ..."
    if fetch "$dest" "$url" && verify_hef "$dest"; then
        local size
        size=$(stat -c%s "$dest" 2>/dev/null || echo "?")
        echo "  OK ($(numfmt --to=iec "$size" 2>/dev/null || echo "${size} bytes"))"
        inc OK
    else
        rm -f "$dest" 2>/dev/null
        echo "  FAIL"
        inc FAIL
    fi
}

download_multi() {
    local dest="$1"
    shift

    if [[ -f "$dest" ]]; then
        echo "  SKIP (exists)"
        inc SKIP
        return 0
    fi

    for url in "$@"; do
        echo "  Trying $(basename "$url") ..."
        if fetch "$dest" "$url" && verify_hef "$dest"; then
            local size
            size=$(stat -c%s "$dest" 2>/dev/null || echo "?")
            echo "  OK ($(numfmt --to=iec "$size" 2>/dev/null || echo "${size} bytes"))"
            inc OK
            return 0
        fi
        rm -f "$dest" 2>/dev/null
        echo "  no"
    done

    echo "  FAIL (all URLs failed)"
    inc FAIL
}

# Copy a file from a local path. Returns 1 (without counting FAIL) when the
# source is absent — the caller decides whether that is fatal or whether it
# falls back to another source.
copy_local() {
    local src="$1"
    local dest="$2"

    if [[ -f "$dest" ]]; then
        echo "  SKIP (exists)"
        inc SKIP
        return 0
    fi

    if [[ -f "$src" ]]; then
        cp "$src" "$dest"
        echo "  OK (copied from $src)"
        inc OK
        return 0
    fi

    echo "  local source not found: $src"
    return 1
}

# item <title> — gate an entry in --core mode: DPM-critical entries call
# `item x --core`; everything else passes no flag and is skipped with --core.
item() {
    local title="$1"
    local is_core="${2:-}"
    if $CORE_ONLY && [[ "$is_core" != "--core" ]]; then
        echo "$title"
        echo "  SKIP (--core)"
        inc SKIP
        return 1
    fi
    echo "$title"
    return 0
}

echo "============================================"
echo " NeoRuntime Built-in Model Download"
echo "============================================"
echo " Mode: $(${CORE_ONLY} && echo 'core (DPM-critical only)' || echo 'full preload set')"
echo " Destination: $MODEL_BASE"
echo " Transport: $([[ -n "$CURL" ]] && echo curl || echo wget)"
echo ""

# Create directories
for dir in detection classification segmentation keypoint clip depth ocr genai; do
    mkdir -p "$MODEL_BASE/$dir"
done

# --- Tier 1: Confirmed public URLs ---

if item "[1/13] vit_large (classification)"; then
    download "$MODEL_BASE/classification/vit_large.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/vit_large.hef"
fi

if item "[2/13] face_landmarks_lite (keypoint)"; then
    download "$MODEL_BASE/keypoint/face_landmarks_lite.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/face_landmarks_lite.hef"
fi

if item "[3/13] clip_vit_b_32_image_encoder_nv12 (clip)"; then
    download "$MODEL_BASE/clip/clip_vit_b_32_image_encoder_nv12.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/clip_vit_b_32_image_encoder_nv12.hef"
fi

if item "[4/13] clip_vit_b_16_image_encoder (clip)"; then
    download_multi "$MODEL_BASE/clip/clip_vit_b_16_image_encoder.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/clip_vit_b_16_image_encoder.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/clip_vit_b_16_image_encoder.hef"
fi

if item "[5/13] scdepthv3 (depth)"; then
    download "$MODEL_BASE/depth/scdepthv3.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/scdepthv3.hef"
fi

if item "[6/13] paddle_ocr_v5_mobile_detection (ocr)"; then
    download "$MODEL_BASE/ocr/paddle_ocr_v5_mobile_detection.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/paddle_ocr_v5_mobile_detection.hef"
fi

# --- Tier 2: Multiple URL candidates ---

# CORE: DPM-critical. Custom 4-class model (person/vehicle/face/license_plate),
# NOT the coco-80 zoo build — a coco HEF at this path loads fine but its labels
# never match keep_labels, silently disarming the mask (2026-08 field incident).
if item "[7/13] hailo_yolov8n_384_640 (detection, 4-class person/vehicle/face)" --core; then
    download_multi "$MODEL_BASE/detection/hailo_yolov8n_384_640.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n_384_640_nv12.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n_384_640.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n.hef"
fi

# CORE: postproc JSON for the 4-class yolov8n detector. Required by
# camera-daemon DPM (DetectorSpec.post_json → HAL config_file → merged_vendor_json
# → p->labels) so the postprocess populates d.label ("person"/"vehicle"/"face");
# without it d.label is empty and keep_labels string-match never fires. Content
# is the authoritative 4-class label map (verbatim from the known-working shared
# configs/yolov8.json that model-showcase filters label=="face" against).
if item "[7b/13] hailo_yolov8n_384_640 postproc JSON (DPM label map)" --core; then
    if [[ -f "$MODEL_BASE/detection/hailo_yolov8n_384_640.json" ]]; then
        echo "  SKIP (exists)"
        inc SKIP
    else
        cat > "$MODEL_BASE/detection/hailo_yolov8n_384_640.json" <<'EOF'
{
    "iou_threshold": 0.9,
    "detection_threshold": 0.38,
    "output_activation": "none",
    "label_offset": 1,
    "max_boxes": 10000,
    "labels": ["unlabeled", "person", "vehicle", "face", "license_plate"]
}
EOF
        echo "  OK (generated)"
        inc OK
    fi
fi

if item "[8/13] yolov5m_vehicles (detection)"; then
    download_multi "$MODEL_BASE/detection/yolov5m_vehicles.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/yolov5m_vehicles.hef" \
        "$S3_BASE/HailoNets/LPR/vehicle_detector/yolov5m_vehicles/hailo15h/2026-01-06/yolov5m_vehicles.hef"
fi

# CORE: plate detector for the license_plate mask channel (grid output, no
# JSON needed — DPM decodes the raw tensors itself).
if item "[9/13] tiny_yolov4_license_plates (detection)" --core; then
    download_multi "$MODEL_BASE/detection/tiny_yolov4_license_plates.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/tiny_yolov4_license_plates.hef" \
        "$S3_BASE/HailoNets/LPR/lp_detector/tiny_yolov4_license_plates/hailo15h/2025-09-17/tiny_yolov4_license_plates.hef"
fi

if item "[10/13] lprnet (ocr)"; then
    download_multi "$MODEL_BASE/ocr/lprnet.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/lprnet.hef" \
        "$S3_BASE/HailoNets/LPR/ocr/lprnet/hailo15h/2025-09-17/lprnet.hef"
fi

if item "[11/13] paddle_ocr_v5_mobile_recognition_nv12 (ocr)"; then
    download_multi "$MODEL_BASE/ocr/paddle_ocr_v5_mobile_recognition_nv12.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/paddle_ocr_v5_mobile_recognition_nv12.hef" \
        "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/paddle_ocr_v5_mobile_recognition_nv12.hef"
fi

# --- Tier 3: Special handling ---

# CORE: person silhouette segmentation. VERIFIED MISMATCH WARNING: the public
# zoo v5.2.0 build (md5 c4ae8b58…) is a DIFFERENT compile than the factory
# preload build (a8384d45… measured on a running device) — a swapped build at
# this path loads fine but silently breaks the person channel (the 2026-08
# incident pattern), so the zoo is NOT a trusted source here. Accept only:
#   1. the copy bundled with the legacy DPM reference app, or
#   2. an operator-provided source via LINKNET_SRC (file path or URL), e.g.
#      copied from another factory device.
if item "[12/13] linknet_mbv1_ss_dpm_256 (segmentation)" --core; then
    LINKNET_DEST="$MODEL_BASE/segmentation/linknet_mbv1_ss_dpm_256.hef"
    # Revalidate a file that is already in place: a device that ran the old
    # downloader may carry the zoo build at this exact path, and "exists" is
    # not proof of trust — quarantine anything that fails the factory digest
    # before looking for a trusted source.
    if [[ -f "$LINKNET_DEST" ]] && ! verify_linknet "$LINKNET_DEST"; then
        echo "  existing file is NOT the factory build (md5 mismatch) — moving it aside"
        mv -f "$LINKNET_DEST" "$LINKNET_DEST.unverified-bak"
    fi
    linknet_got=false
    if [[ -f "$LINKNET_DEST" ]]; then
        echo "  SKIP (exists, factory build verified)"
        inc SKIP
        linknet_got=true
    elif [[ -f "$LOCAL_LINKNET" ]]; then
        cp "$LOCAL_LINKNET" "$LINKNET_DEST"
        if verify_linknet "$LINKNET_DEST"; then
            echo "  OK (copied from legacy DPM app, factory build verified)"
            inc OK
            linknet_got=true
        else
            rm -f "$LINKNET_DEST"
            echo "  legacy-app copy failed factory-build verification"
        fi
    fi
    if ! $linknet_got && [[ -n "${LINKNET_SRC:-}" ]]; then
        case "$LINKNET_SRC" in
            http://*|https://*)
                echo "  downloading operator-provided $LINKNET_SRC ..."
                download "$LINKNET_DEST" "$LINKNET_SRC"
                if [[ -f "$LINKNET_DEST" ]] && verify_linknet "$LINKNET_DEST"; then
                    linknet_got=true
                fi
                ;;
            *)
                if [[ -f "$LINKNET_SRC" ]]; then
                    echo "  copying operator-provided $LINKNET_SRC ..."
                    cp "$LINKNET_SRC" "$LINKNET_DEST"
                    if verify_linknet "$LINKNET_DEST"; then
                        echo "  OK (operator-provided source, factory build verified)"
                        inc OK
                        linknet_got=true
                    fi
                else
                    echo "  local source not found: $LINKNET_SRC"
                fi
                ;;
        esac
        if ! $linknet_got && [[ -f "$LINKNET_DEST" ]]; then
            # A source produced a file that failed the factory digest.
            rm -f "$LINKNET_DEST"
            echo "  FAIL (source did not pass factory-build verification)"
        fi
    fi
    if ! $linknet_got && [[ ! -f "$LINKNET_DEST" ]]; then
        echo "  FAIL (no trusted source delivered the factory build)"
        echo "  The public model zoo build is NOT the factory build (md5 mismatch"
        echo "  verified on-device) and would silently break the person channel."
        echo "  Provide the factory file: LINKNET_SRC=/path/or/url bash $0 --core"
        echo "  (e.g. scp it from a factory device) and re-run."
        inc FAIL
    fi
fi

if item "[13/13] Qwen3-VL-2B-Instruct (genai, ~3GB)"; then
    if $WITH_GENAI; then
        download "$MODEL_BASE/genai/Qwen3-VL-2B-Instruct.hef" \
            "$GENAI_BASE/Qwen3-VL-2B-Instruct.hef"
    else
        echo "  SKIP (opt-in: pass --with-genai)"
        inc SKIP
    fi
fi

# --- Summary ---

echo ""
echo "============================================"
echo " Summary: OK=$OK  FAIL=$FAIL  SKIP=$SKIP"
echo "============================================"

# Name the DPM-critical files that are still missing: camera-daemon hardcodes
# these exact paths, and any one of them being absent silently disarms that AI
# privacy-mask channel on the device (armed toggle, no mask).
dpm_missing=""
for f in detection/hailo_yolov8n_384_640.hef \
         detection/hailo_yolov8n_384_640.json \
         segmentation/linknet_mbv1_ss_dpm_256.hef \
         detection/tiny_yolov4_license_plates.hef; do
    [[ -f "$MODEL_BASE/$f" ]] || dpm_missing="${dpm_missing}  - MISSING: $f"$'\n'
done
if [[ -n "$dpm_missing" ]]; then
    echo ""
    echo "WARNING: AI privacy mask (DPM) will NOT fully work on this device:"
    printf '%s' "$dpm_missing"
fi

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "Some models failed to download."
    echo "Check network connectivity and retry."
    exit 1
fi

echo ""
echo "Model files:"
find "$MODEL_BASE" -name "*.hef" -exec ls -lh {} \; 2>/dev/null | sort
find "$MODEL_BASE" -name "*.json" -exec ls -lh {} \; 2>/dev/null | sort

# --- Auto-register new models via scan API ---
# POST via curl when available; busybox wget cannot POST, so fall back to the
# on-device python3. Both are best-effort: a miss just means the operator
# should restart platform-api (or re-run) to register the new files.
echo ""
SCAN_URL="http://localhost:8080/api/v1/ai/models/scan"
SCAN_RESULT=""
if [[ -n "$CURL" ]]; then
    SCAN_RESULT=$("$CURL" -s -X POST "$SCAN_URL" -H "Authorization: Bearer "${AIPC_TOKEN_KEY:-}"" 2>/dev/null || true)
elif command -v python3 >/dev/null 2>&1; then
    SCAN_RESULT=$(python3 - "$SCAN_URL" "${AIPC_TOKEN_KEY:-}" <<'PYEOF' 2>/dev/null || true
import sys, urllib.request
url, token = sys.argv[1], sys.argv[2]
req = urllib.request.Request(url, method="POST",
                             headers={"Authorization": "Bearer " + token} if token else {})
with urllib.request.urlopen(req, timeout=10) as r:
    print(r.read().decode())
PYEOF
    )
fi
if [[ -n "$SCAN_RESULT" ]]; then
    echo "Model scan: $SCAN_RESULT"
else
    echo "Note: Could not reach platform-api scan endpoint."
    echo "Run to register: systemctl restart platform-api"
fi
