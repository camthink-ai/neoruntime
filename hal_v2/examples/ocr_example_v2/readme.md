# OCR example (`hal-ocr-example-v2`)

This example implements a two-stage OCR pipeline:

- **OCR detection**: detect text boxes on the full frame
- **OCR recognition**: crop each text ROI and recognize characters (supports NV12/RGB recognition HEFs)

After building, the executable is `hal-ocr-example-v2`.

## Run on device (example commands)

### 1) Classic OCR: det + rec (find text on full frame)

```bash
./hal-ocr-example-v2 \
  --media /etc/imaging/cfg/medialib_configs/ai_example_medialib_config.json --profile 0 \
  --ocr-det-hef ./paddle_ocr_v5_mobile_detection.hef \
  --ocr-rec-hef ./paddle_ocr_v5_mobile_recognition_nv12.hef \
  --rec-charset ./ppocrv5_dict.txt \
  --rec-dict-index-offset 1 \
  --udp 192.0.2.93:5555 --verbose
```

Notes:
- `paddle_ocr_v5_mobile_recognition_nv12.hef`: recognition input is **NV12**
- `paddle_ocr_v5_mobile_recognition.hef`: recognition input is **RGB**
- `--ocr-det-post-file/--ocr-det-post-json` and `--ocr-rec-post-file/--ocr-rec-post-json` are optional, used to provide a vendor postprocess config file/JSON (if omitted, the example uses built-in defaults)

### 2) Recommended for license plates: plate detection + OCR recognition (skip OCR-det)

For license plate use-cases, it is usually better to run a **plate detection model** to get the plate ROI, and then run **OCR recognition only** (avoids false positives and the extra compute cost of full-frame OCR-det).

This pipeline is integrated in `hal-lpr-example-v2` under `examples/lpr_example_v2` (use `--rec-backend ppocrv5`). See:

- `examples/lpr_example_v2/readme.md`
- `examples/lpr_example_v2/data/ppocrv5_recognition.json`

Key pieces:
- Detection: `tiny_yolov4_license_plates.hef` (produces plate bounding boxes)
- Recognition: `paddle_ocr_v5_mobile_recognition_nv12.hef` + `libocr_post.so:ocr_postprocess` + `ppocrv5_dict.txt`

