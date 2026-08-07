#!/bin/bash
# download_models.sh — Download model-showcase HEF models from Hailo official model zoo
#
# Usage:
#   bash download_models.sh              # Download all models (including genai)
#   bash download_models.sh --skip-genai # Skip GenAI model (~3GB)
#
# Run this script on the Hailo device.

set -uo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
SCRIPT_ROOT="$(dirname "$SELF_DIR")"

# Parse arguments
WITH_GENAI=true
DEST_ARG=""
shift_next=0
for arg in "$@"; do
    case "$arg" in
        --skip-genai) WITH_GENAI=false ;;
        --dest) shift_next=1 ;;
        *)
            if [[ "$shift_next" == "1" ]]; then
                DEST_ARG="$arg"; shift_next=0
            fi
            ;;
    esac
done

MODEL_BASE="${DEST_ARG:-${INSTALL_PREFIX:-${SCRIPT_ROOT}}/models}"
S3_BASE="https://hailo-model-zoo.s3.eu-west-2.amazonaws.com"
GENAI_BASE="https://dev-public.hailo.ai/v5.3.0/blob"
LOCAL_LINKNET="/home/root/apps/dynamic_privacy_mask/resources/linknet_mbv1_ss_dpm_256.hef"

OK=0
FAIL=0
SKIP=0

inc() { eval "$1=\$((\$$1 + 1))"; }

download() {
    local dest="$1"
    local url="$2"

    if [[ -f "$dest" ]]; then
        echo "  SKIP (exists)"
        inc SKIP
        return 0
    fi

    echo "  Downloading ..."
    if curl --fail --location --progress-bar --output "$dest" "$url"; then
        local size
        size=$(stat -c%s "$dest" 2>/dev/null || echo "?")
        echo "  OK ($(numfmt --to=iec "$size" 2>/dev/null || echo "${size} bytes"))"
        inc OK
        return 0
    else
        rm -f "$dest" 2>/dev/null
        echo "  FAIL"
        inc FAIL
        return 0
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
        if curl --fail --location --progress-bar --output "$dest" "$url"; then
            local size
            size=$(stat -c%s "$dest" 2>/dev/null || echo "?")
            echo "  OK ($(numfmt --to=iec "$size" 2>/dev/null || echo "${size} bytes"))"
            inc OK
            return 0
        else
            rm -f "$dest" 2>/dev/null
            echo "  no"
        fi
    done

    echo "  FAIL (all URLs failed)"
    inc FAIL
    return 0
}

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
    else
        echo "  FAIL (source not found: $src)"
        inc FAIL
    fi
}

echo "============================================"
echo " NE503 Model Showcase — Model Download"
echo "============================================"
echo ""

# Create directories
for dir in detection classification segmentation keypoint clip depth ocr genai; do
    mkdir -p "$MODEL_BASE/$dir"
done

# --- Tier 1: Confirmed public URLs ---

echo "[1/13] vit_large (classification)"
download "$MODEL_BASE/classification/vit_large.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/vit_large.hef"

echo "[2/13] face_landmarks_lite (keypoint)"
download "$MODEL_BASE/keypoint/face_landmarks_lite.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/face_landmarks_lite.hef"

echo "[3/13] clip_vit_b_32_image_encoder_nv12 (clip)"
download "$MODEL_BASE/clip/clip_vit_b_32_image_encoder_nv12.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/clip_vit_b_32_image_encoder_nv12.hef"

echo "[4/13] clip_vit_b_16_image_encoder (clip)"
download_multi "$MODEL_BASE/clip/clip_vit_b_16_image_encoder.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/clip_vit_b_16_image_encoder.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/clip_vit_b_16_image_encoder.hef"

echo "[5/13] scdepthv3 (depth)"
download "$MODEL_BASE/depth/scdepthv3.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/scdepthv3.hef"

echo "[6/13] paddle_ocr_v5_mobile_detection (ocr)"
download "$MODEL_BASE/ocr/paddle_ocr_v5_mobile_detection.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/paddle_ocr_v5_mobile_detection.hef"

# --- Tier 2: Multiple URL candidates ---

echo "[7/13] hailo_yolov8n_384_640 (detection, 4-class person/vehicle/face)"
download_multi "$MODEL_BASE/detection/hailo_yolov8n_384_640.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n_384_640_nv12.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n_384_640.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/hailo_yolov8n.hef"

# Postproc JSON for the 4-class yolov8n detector. Required by camera-daemon DPM
# (DetectorSpec.post_json → HAL config_file → merged_vendor_json → p->labels) so
# the postprocess populates d.label ("person"/"vehicle"/"face"); without it
# d.label is empty and keep_labels string-match never fires. Content is the
# authoritative 4-class label map (verbatim from the known-working shared
# configs/yolov8.json that ne503-aipc-apps/showcases/model-showcase filters label=="face" against).
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

echo "[8/13] yolov5m_vehicles (detection)"
download_multi "$MODEL_BASE/detection/yolov5m_vehicles.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/yolov5m_vehicles.hef" \
    "$S3_BASE/HailoNets/LPR/vehicle_detector/yolov5m_vehicles/hailo15h/2026-01-06/yolov5m_vehicles.hef"

echo "[9/13] tiny_yolov4_license_plates (detection)"
download_multi "$MODEL_BASE/detection/tiny_yolov4_license_plates.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/tiny_yolov4_license_plates.hef" \
    "$S3_BASE/HailoNets/LPR/lp_detector/tiny_yolov4_license_plates/hailo15h/2025-09-17/tiny_yolov4_license_plates.hef"

echo "[10/13] lprnet (ocr)"
download_multi "$MODEL_BASE/ocr/lprnet.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/lprnet.hef" \
    "$S3_BASE/HailoNets/LPR/ocr/lprnet/hailo15h/2025-09-17/lprnet.hef"

echo "[11/13] paddle_ocr_v5_mobile_recognition_nv12 (ocr)"
download_multi "$MODEL_BASE/ocr/paddle_ocr_v5_mobile_recognition_nv12.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.2.0/hailo15h/paddle_ocr_v5_mobile_recognition_nv12.hef" \
    "$S3_BASE/ModelZoo/Compiled/v5.3.0/hailo15h/paddle_ocr_v5_mobile_recognition_nv12.hef"

# --- Tier 3: Special handling ---

echo "[12/13] linknet_mbv1_ss_dpm_256 (segmentation)"
copy_local "$LOCAL_LINKNET" "$MODEL_BASE/segmentation/linknet_mbv1_ss_dpm_256.hef"

echo "[13/13] Qwen3-VL-2B-Instruct (genai)"
if $WITH_GENAI; then
    download "$MODEL_BASE/genai/Qwen3-VL-2B-Instruct.hef" \
        "$GENAI_BASE/Qwen3-VL-2B-Instruct.hef"
else
    echo "  SKIP (downloaded by default, use --skip-genai to skip)"
    inc SKIP
fi

# --- Summary ---

echo ""
echo "============================================"
echo " Summary: OK=$OK  FAIL=$FAIL  SKIP=$SKIP"
echo "============================================"

if [[ $FAIL -gt 0 ]]; then
    echo ""
    echo "Some models failed to download."
    echo "Check network connectivity and retry."
    exit 1
fi

echo ""
echo "Model files:"
find "$MODEL_BASE" -name "*.hef" -exec ls -lh {} \; 2>/dev/null | sort

# --- Auto-register new models via scan API ---
echo ""
SCAN_URL="http://localhost:8080/api/v1/ai/models/scan"
SCAN_RESULT=$(curl -s -X POST "$SCAN_URL" -H "Authorization: Bearer "${AIPC_TOKEN_KEY:-}"" 2>/dev/null)
if [[ -n "$SCAN_RESULT" ]]; then
    echo "Model scan: $SCAN_RESULT"
else
    echo "Note: Could not reach platform-api scan endpoint."
    echo "Run to register: systemctl restart platform-api"
fi
