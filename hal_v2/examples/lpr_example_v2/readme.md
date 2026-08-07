# License plate recognition example (`hal-lpr-example-v2`)

Two-stage pipeline aligned with Hailo Model Zoo and the [hailo-camera-apps LPR GStreamer script](https://github.com/hailo-ai/hailo-camera-apps/blob/e9957a349c7f6db701140e7e52e84a5418177552/apps/h15/gstreamer/license_plate_recognition/debug/lpr_small_yolo_rgb.sh) (`lpr_small_yolo_rgb.sh`).

### Mapping to `lpr_small_yolo_rgb.sh` (around L31)

The plate-detection postprocess in the script maps to:

| GStreamer `hailofilter` | HAL `hal-lpr-example-v2` / `HalPostprocessConfig` (detection) |
|-------------------------|-------------------------------------------------------------|
| `so-path=$POSTPROCESS_DIR/libyolo_post.so` | `backend_lib_path` (in JSON, or HAL default) |
| `config-path=$license_plate_json_config_path` | The **actual JSON validated by the plugin** (see below) |
| `function-name=tiny_yolov4_license_plates` | `backend_function`: `tiny_yolov4_license_plates` |

Key point: **GStreamer passes the `.so` path and the "network postprocess JSON" as two separate parameters**. HAL typically puts `backend_*` and YOLO parameters in one JSON. If you still hit schema errors with the merged file, follow the official split exactly:

1. **Recommended (1:1 semantics with the script)**  
   On the device, locate the exact **`yolov4_license_plate.json`** referenced by GStreamer `config-path` (usually shipped in the image / SDK sample resources, alongside `lpr_small_yolo_rgb.sh` under `resources/configs/`; GitHub archives often **don't include** `resources/`, so copy it from the image or full package).  
   Use [`data/hal_lpr_det_wrapper.example.json`](data/hal_lpr_det_wrapper.example.json): keep only `backend_lib_path`, `backend_function`, and `backend_config_path` (pointing to the device's **pure YOLO** JSON) as `--lp-det-post-file`.  
   This ensures the file passed to `init()` is **exactly the same** as the script's `config-path=.../yolov4_license_plate.json`, avoiding schema mismatches in `libyolo_post.so`.

2. **Single-file mode**  
   `data/yolov4_license_plate.json` is a simplified self-contained example for HAL. If you still see JSON/schema errors, replace the **YOLO parameter section** with the official device JSON (keep `backend_*`), or upgrade to a `libaipc_hal.so` that already strips `backend_*` before writing the temp config passed to the vendor plugin.

Official script snippet (logical mapping): `LICENSE_PLATE_DETECTION_POST_SO` / `DEFAULT_LICENSE_PLATE_JSON_CONFIG_PATH` / `LICENSE_PLATE_DETECTION_POST_FUNC` in [`lpr_small_yolo_rgb.sh` (raw)](https://raw.githubusercontent.com/hailo-ai/hailo-camera-apps/repo-is-archived/apps/h15/gstreamer/license_plate_recognition/debug/lpr_small_yolo_rgb.sh).

1. **Detection** — `tiny_yolov4_license_plates` (416×416 RGB). Post: `libyolo_post.so`, function `tiny_yolov4_license_plates`. [Model zoo](https://github.com/hailo-ai/hailo_model_zoo/tree/master/hailo_models/license_plate_detection).
2. **Recognition** — two backends are supported:
   - **`lprnet`** (75×300 RGB, 10 digits + blank, CTC; this example defaults to local decoding)
   - **`ppocrv5`** (PaddleOCR v5 recognition; recommended to use an NV12 HEF + `libocr_post.so:ocr_postprocess`, supports Chinese/letters/digits; dictionary `ppocrv5_dict.txt`)

## SDK / cross-build

Source the Poky SDK environment, then configure HAL so `CMAKE_SYSROOT` points at `armv8a-poky-linux`:

```bash
source ../sdk_4.0.23/environment-setup-armv8a-poky-linux
cd hal_v2 && mkdir -p build-hailo15 && cd build-hailo15
cmake .. -DHAL_PLATFORM=hailo15
cmake --build . --target hal-lpr-example-v2
```

`hal_v2/CMakeLists.txt` also searches `../sdk_4.0.23/sysroots/armv8a-poky-linux` when `OECORE_TARGET_SYSROOT` is not set.

## Run on device

Copy compiled `hal-lpr-example-v2`, HEFs from the model zoo, and JSON configs (adjust `backend_*` paths if your rootfs differs). Example:

```bash
./hal-lpr-example-v2 \
  --media /path/to/medialib.json --profile 0 \
  --lp-det-hef /path/to/tiny_yolov4_license_plates.hef \
  --lp-det-post-file /path/to/yolov4_license_plate.json \
  --lp-rec-hef /path/to/lprnet.hef \
  --lp-rec-post-file /path/to/lprnet_recognition.json \
  --udp 127.0.0.1:5004 --verbose --max-plates 4
```

Omit `--lp-rec-hef` to run **detection only** (bounding boxes on stream).

### Run with PaddleOCR v5 recognition (NV12) for plate text

Use YOLO plate detection, then run **PaddleOCR recognition** on the plate ROI (no OCR-det stage):

```bash
./hal-lpr-example-v2 \
  --media /path/to/medialib.json --profile 0 \
  --lp-det-hef /path/to/tiny_yolov4_license_plates.hef \
  --lp-det-post-file /path/to/yolov4_license_plate.json \
  --lp-rec-hef /path/to/paddle_ocr_v5_mobile_recognition_nv12.hef \
  --lp-rec-post-file /path/to/ppocrv5_recognition.json \
  --rec-backend ppocrv5 --rec-charset /path/to/ppocrv5_dict.txt \
  --udp 127.0.0.1:5004 --verbose --max-plates 4
```

Starter JSON for PaddleOCR recognition is `data/ppocrv5_recognition.json`.

Starter JSON files live in `data/`; validate `backend_function` names against the postprocess libraries shipped on your image (`strings /usr/lib/hailo-post-processes/libocr_post.so` etc. if needed).

### `libyolo_post.so` / `json config file doesn't follow schema rules`

HAL merges `backend_lib_path`, `backend_function`, and optional `backend_config_path` into one JSON blob. If `backend_config_path` is omitted, HAL used to pass that **entire** blob to the plugin `init(config_path, …)`, and YOLO’s schema rejects the `backend_*` keys.

**Fix (HAL):** from this revision onward, those three keys are stripped from the temp file passed to `init()`. Rebuild `libaipc_hal.so` / `hal` and redeploy.

**Alternative (no HAL upgrade):** split config — outer JSON with only `backend_*` pointing at `backend_config_path`, and a second file with **only** YOLO parameters (thresholds, labels, anchors, …) as shipped with hailo-camera-apps / model zoo.
