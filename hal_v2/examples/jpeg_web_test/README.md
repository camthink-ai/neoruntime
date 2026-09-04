# hal-jpeg-web-test — HAL JPEG/MJPEG encoding verification (web viewer)

End-to-end example for the HAL JPEG encoding chain: **single-frame encoding** +
**continuous MJPEG stream**, with a built-in HTTP server for viewing directly in a browser.

> **Platform fact (important)**: on Hailo-15, **JPEG/MJPEG is CPU software encoding**, not
> hardware encoding. The official Media Library user guide states explicitly: "JPEG Encoding
> is not hardware accelerated"; only H.264/H.265 go through the VCENC hardware encoder. The
> HAL's `HAL_PACKET_TYPE_MJPEG` path (configurable quality, medialib `jpeg_encoder_config_t`,
> multi-threaded CPU JPEG underneath) — this example verifies the HAL wrapper correctness of
> that **software chain**, not hardware acceleration.

## Build

```bash
# hailo15 cross-compile (from hal_v2/)
source /home/wicevi/hailo_env/sdk_4.0.23/environment-setup-armv8a-poky-linux
cmake --build build-hailo15 -j    # artifact: build-hailo15/hal-jpeg-web-test
```

The stub platform has no medialib; this example is excluded from the stub build
(it needs real media hardware).

## Run

```bash
# after deploying to the board (libaipc_hal.so must be on the ld path)
./hal-jpeg-web-test [port=8080] [width=1280] [height=720] [fps=10] [quality=85]

# e.g. 1080p, quality 90
./hal-jpeg-web-test 8080 1920 1080 10 90
```

Flow: start the pipeline from the embedded default profile -> `add_codec_stream` adds an
MJPEG stream -> subscribe to encoded packets -> **first frame automatically saved to
`/tmp/hal_jpeg_snap.jpg` (single-frame verification)** -> start the HTTP server.

## Web endpoints

| URL | Description |
|---|---|
| `http://<board-ip>:8080/` | Viewer page (stream + snapshot links + live stats) |
| `/stream` | MJPEG stream (multipart/x-mixed-replace; display directly with `<img>`) |
| `/snap.jpg` | Latest JPEG frame |
| `/snap/new` | Capture the **next frame**: writes `/tmp/hal_jpeg_snap.jpg` and returns that frame |
| `/stats` | JSON: `{"frames","fps","last_frame_bytes","total_bytes"}` |

## Acceptance checks

1. **Single frame**: after `/snap/new`, `ls -la /tmp/hal_jpeg_snap.jpg`, scp it out and open
   in an image viewer — picture normal, dimensions correct
2. **Stream**: opening `/` in a browser shows continuous motion; `/stats` fps close to the
   configured value (software encode; CPU usage rises with resolution/quality)
3. **Quality steps**: run once each with `quality` 45 and 95, compare `/snap.jpg` sizes
   (verifies `jpeg_quality` passthrough)
4. **Resolution steps**: run once each at 720p and 1080p (4K software encoding stresses the
   CPU; not recommended)

Ctrl+C exits (the pipeline is destroyed with the process).
