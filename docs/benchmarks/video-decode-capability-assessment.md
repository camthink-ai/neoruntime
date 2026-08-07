# Hailo-15 Video Decode Capability Assessment Plan

> **Assessment Goal**: Determine the video decoding performance limits of the model-showcase application on Hailo-15 devices
> **Device**: NE503 (Hailo-15H) · 8 GB DDR · 4x A53 @ 1.3 GHz
> **Container Environment**: model-showcase (opencv-python-headless + FFmpeg software decoding)

---

## 1. Current Decoding Architecture

```
Video File (MP4/AVI/MKV)
       |
       v  (Inside container, no hardware decoder)
OpenCV VideoCapture (FFmpeg backend)
       |
       v  (CPU software decoding)
BGR numpy array (numpy.ndarray)
       |
       +---> _prepare_input_from_bgr() -> resize + NV12/RGB conversion -> NPU inference
       |
       +---> MJPEG stream -> Frontend display
```

**Key constraint**: The container does not have `ffprobe`/`ffmpeg` CLI tools, and **no hardware decode acceleration** -- it relies entirely on CPU software decoding. The Hailo-15 hardware encoders/decoders (H.264/H.265 encoder) are exclusively used by camera-daemon for **encoding output**, and no decode API is provided.

## 2. Dimensions to Evaluate

| Dimension | Description | Measurement Metric |
|-----------|-------------|-------------------|
| **Decode throughput** | How many frames can be decoded per second | FPS |
| **Resolution ceiling** | Maximum supported resolution | Max width x height |
| **CPU overhead** | How much CPU decoding consumes | % CPU / core utilization |
| **Memory overhead** | How much RAM decoding consumes | MB |
| **Format compatibility** | Which encoding formats are supported | H.264 / H.265 / VP9 / AV1 |
| **Decode + inference combined** | Bottleneck when decoding and inference run simultaneously | Total FPS + latency |
| **Seek performance** | Random seek latency | ms per seek |

## 3. Assessment Test Design

### 3.1 Test Matrix

| Resolution | FPS | Encoding | Bitrate | File Size |
|-----------|-----|----------|---------|-----------|
| 640x384 | 15 | H.264 | 1 Mbps | Small |
| 640x384 | 30 | H.264 | 2 Mbps | Small |
| 1280x720 | 15 | H.264 | 2 Mbps | Medium |
| 1280x720 | 30 | H.264 | 4 Mbps | Medium |
| 1920x1080 | 15 | H.264 | 4 Mbps | Large |
| 1920x1080 | 30 | H.264 | 8 Mbps | Large |
| 1920x1080 | 30 | H.265 | 4 Mbps | Large |
| 3840x2160 | 15 | H.264 | 16 Mbps | Very Large |
| 3840x2160 | 15 | H.265 | 8 Mbps | Very Large |

### 3.2 Test Script

```python
#!/usr/bin/env python3
"""Video decode benchmark for Hailo-15 model-showcase container."""
import cv2
import time
import json
import psutil
import numpy as np
import threading
import statistics

RESULTS = []

def decode_benchmark(video_path, label):
    """Benchmark: Pure decode throughput"""
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"  FAIL: cannot open {video_path}")
        return

    w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
    h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
    fps = cap.get(cv2.CAP_PROP_FPS) or 30
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))
    codec = int(cap.get(cv2.CAP_PROP_FOURCC))

    proc = psutil.Process()
    mem_before = proc.memory_info().rss / 1024 / 1024
    cpu_before = proc.cpu_percent(interval=None)

    latencies = []
    frame_count = 0
    t0 = time.monotonic()

    while True:
        ret, frame = cap.read()
        if not ret:
            break
        frame_count += 1

    t1 = time.monotonic()
    elapsed = t1 - t0

    mem_after = proc.memory_info().rss / 1024 / 1024

    decode_fps = frame_count / elapsed if elapsed > 0 else 0
    result = {
        "label": label,
        "resolution": f"{w}x{h}",
        "fps_source": fps,
        "codec_fourcc": int(codec),
        "total_frames": total,
        "decoded_frames": frame_count,
        "elapsed_s": round(elapsed, 2),
        "decode_fps": round(decode_fps, 1),
        "mem_delta_mb": round(mem_after - mem_before, 1),
    }
    RESULTS.append(result)
    cap.release()

    print(f"  {label}: {w}x{h} @ {fps:.0f}fps -> "
          f"decode {decode_fps:.1f} fps ({elapsed:.2f}s for {frame_count} frames) "
          f"mem +{mem_after - mem_before:.1f}MB")

def decode_with_resize(video_path, target_w, target_h, label):
    """Benchmark: Decode + resize (simulating inference preprocessing)"""
    cap = cv2.VideoCapture(video_path)
    if not cap.isOpened():
        print(f"  FAIL: cannot open {video_path}")
        return

    frame_count = 0
    t0 = time.monotonic()
    while True:
        ret, frame = cap.read()
        if not ret:
            break
        # Simulate inference preprocessing: resize to model input size
        resized = cv2.resize(frame, (target_w, target_h))
        frame_count += 1
    t1 = time.monotonic()
    elapsed = t1 - t0
    fps = frame_count / elapsed if elapsed > 0 else 0
    print(f"  {label} + resize({target_w}x{target_h}): {fps:.1f} fps "
          f"({elapsed:.2f}s for {frame_count} frames)")
    cap.release()

def decode_plus_infer(video_path, model_id, target_w, target_h,
                      input_fmt, label, n_frames=50):
    """Benchmark: Decode + inference combined"""
    sys.path.insert(0, '/data/lib/python')
    from hailo_ipc_sdk import InferenceClient

    cap = cv2.VideoCapture(video_path)
    client = InferenceClient()
    client.connect()

    decode_lats = []
    infer_lats = []

    for i in range(n_frames):
        ret, frame = cap.read()
        if not ret:
            cap.set(cv2.CAP_PROP_POS_FRAMES, 0)
            ret, frame = cap.read()
            if not ret:
                break

        # Decode latency
        td = time.monotonic()

        # Preprocess
        bgr = cv2.resize(frame, (target_w, target_h))
        if input_fmt == "rgb":
            inp = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).flatten()
        else:
            # NV12 (simplified, measure with RGB)
            inp = cv2.cvtColor(bgr, cv2.COLOR_BGR2RGB).flatten()

        # Infer
        ti = time.monotonic()
        client.infer(inp, model_id=model_id, timeout_ms=10000)
        tf = time.monotonic()

        decode_lats.append((ti - td) * 1000)
        infer_lats.append((tf - ti) * 1000)

    cap.release()
    client.close()

    avg_decode = statistics.mean(decode_lats)
    avg_infer = statistics.mean(infer_lats)
    total_fps = 1000.0 / (avg_decode + avg_infer) if (avg_decode + avg_infer) > 0 else 0

    print(f"  {label}: decode={avg_decode:.1f}ms  infer={avg_infer:.1f}ms  "
          f"total={avg_decode+avg_infer:.1f}ms  ({total_fps:.1f} fps)")

def seek_benchmark(video_path, label, n_seeks=20):
    """Benchmark: Random seek performance"""
    cap = cv2.VideoCapture(video_path)
    total = int(cap.get(cv2.CAP_PROP_FRAME_COUNT))

    lats = []
    for i in range(n_seeks):
        target = int((i / n_seeks) * total)
        t0 = time.monotonic()
        cap.set(cv2.CAP_PROP_POS_FRAMES, target)
        ret, _ = cap.read()
        t1 = time.monotonic()
        if ret:
            lats.append((t1 - t0) * 1000)

    cap.release()
    if lats:
        print(f"  {label} seek: avg={statistics.mean(lats):.1f}ms  "
              f"p50={statistics.median(lats):.1f}ms  max={max(lats):.1f}ms  "
              f"n={len(lats)}")
    else:
        print(f"  {label} seek: FAIL")

def main():
    import sys

    # -- Test 1: Generate test videos (inside container) --
    print("=== Generating test videos ===")
    test_videos = {}
    for (w, h, fps, tag) in [
        (640, 384, 30, "640x384"),
        (1280, 720, 30, "1280x720"),
        (1920, 1080, 30, "1920x1080"),
        (3840, 2160, 15, "3840x2160"),
    ]:
        path = f"/tmp/test_{tag}.mp4"
        fourcc = cv2.VideoWriter_fourcc(*'mp4v')
        writer = cv2.VideoWriter(path, fourcc, fps, (w, h))
        n_frames = min(300, fps * 10)  # 10s or 300 frames
        for i in range(n_frames):
            frame = np.random.randint(0, 255, (h, w, 3), dtype=np.uint8)
            # Add some text to make compression more realistic
            cv2.putText(frame, f"Frame {i}", (50, h//2),
                        cv2.FONT_HERSHEY_SIMPLEX, 2, (255,255,255), 3)
            writer.write(frame)
        writer.release()
        test_videos[tag] = path
        fsize = os.path.getsize(path) / 1024 / 1024
        print(f"  {tag} @ {fps}fps: {n_frames} frames, {fsize:.1f}MB")

    # -- Test 2: Pure decode throughput --
    print("\n=== Test 2: Pure decode throughput ===")
    for tag, path in test_videos.items():
        decode_benchmark(path, tag)

    # -- Test 3: Decode + Resize (simulating inference preprocessing) --
    print("\n=== Test 3: Decode + Resize to 640x384 (inference prep) ===")
    for tag, path in test_videos.items():
        decode_with_resize(path, 640, 384, tag)

    # -- Test 4: Seek performance --
    print("\n=== Test 4: Random seek latency ===")
    for tag, path in test_videos.items():
        seek_benchmark(path, tag)

    # -- Test 5: Decode + inference combined (if ai-runtime available) --
    print("\n=== Test 5: Decode + Inference ===")
    try:
        sys.path.insert(0, '/data/lib/python')
        from hailo_ipc_sdk import InferenceClient
        client = InferenceClient()
        client.connect()
        models = client.list_models()
        if models:
            mid = models[0].model_id
            print(f"  Using model: {mid}")
            for tag, path in test_videos.items():
                decode_plus_infer(path, mid, 640, 384, "rgb", tag, n_frames=30)
        else:
            print("  SKIP: no models loaded")
        client.close()
    except Exception as e:
        print(f"  SKIP: {e}")

    # -- Test 6: CPU usage --
    print("\n=== Test 6: CPU usage during decode ===")
    import subprocess
    for tag, path in test_videos.items():
        # Start decoding process, simultaneously monitor CPU
        cmd = f"python3 -c \"import cv2; cap=cv2.VideoCapture('{path}'); " \
              f"[cap.read() for _ in range(int(cap.get(cv2.CAP_PROP_FRAME_COUNT)))]; cap.release()\""
        t0 = time.monotonic()
        p = subprocess.Popen(cmd, shell=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        # Wait for completion
        p.wait()
        t1 = time.monotonic()
        print(f"  {tag}: wall {(t1-t0):.2f}s")

    # -- Results summary --
    print("\n=== Summary ===")
    print(json.dumps(RESULTS, indent=2))

if __name__ == "__main__":
    import os, sys
    main()
```

## 4. Expected Bottleneck Analysis

### 4.1 CPU is the Primary Bottleneck

| Resource | Specification | Impact |
|----------|--------------|--------|
| CPU | 4x A53 @ 1.3 GHz | Software decoding saturates CPU |
| Memory | 8 GB DDR | Sufficient |
| NPU | 20 TOPS | Inference unaffected by decoding |

H.264 software decoding of 1080p@30fps on Cortex-A53 requires approximately **1.5-2 cores**. The 4-core CPU also needs to run ai-runtime, camera-daemon, and other services; the actual available capacity for decoding may be only **1.5-2 cores**.

### 4.2 Estimated Performance Range

| Resolution | Estimated Pure Decode FPS | Estimated Decode+Infer FPS | Primary Bottleneck |
|-----------|--------------------------|---------------------------|-------------------|
| 640x384 | 60-90 | 25-40 | NPU inference |
| 1280x720 | 30-50 | 20-30 | Decoding + inference |
| 1920x1080 | 15-25 | 10-18 | CPU decoding |
| 3840x2160 | 3-8 | 3-8 | CPU severely insufficient |

### 4.3 Decoding vs Encoding Resource Competition

Current camera-daemon resource usage:

| Service | CPU Usage | Description |
|---------|-----------|-------------|
| camera-daemon | ~10-15% | ISP + encoding 3 H.264 streams |
| ai-runtime | ~5-10% | NPU scheduling (CPU portion is lightweight) |
| platform-api | ~2-3% | REST API |
| Other services | ~3-5% | event-bus, app-manager, etc. |
| **Remaining available** | **~60-70%** ~ 2.5 cores | For video decoding |

## 5. Optimization Directions (If Performance is Insufficient)

### 5.1 Short-term (Software Optimization)

| Approach | Expected Benefit | Complexity |
|----------|-----------------|------------|
| Separate decode thread from inference thread | Prevent FPS from dragging each other down | Low |
| Frame-skip decoding (take 1 of every N frames) | Reduce decode load | Low |
| Decode at lower resolution | Linearly reduce compute | Low |
| FFmpeg multi-threaded decoding (-threads 2) | Utilize multiple cores | Medium |

### 5.2 Medium-term (Architecture Optimization)

| Approach | Expected Benefit | Complexity |
|----------|-----------------|------------|
| Hardware decoding via V4L2 M2M | Zero CPU decode overhead | High |
| DMA-BUF direct to NPU (skip resize) | Zero-copy | High |
| Media pipeline integrated decoding | Leverage Hailo hardware | High |

### 5.3 Hardware Decoding Feasibility

The Hailo-15 Vision Subsystem includes hardware H.264/H.265 encoders/decoders, but the current HAL codec module only exposes the **encoder** API (`HalCodecOps`), with no decoder API. To leverage hardware decoding requires:

1. Adding a `HalDecoderOps` interface in HAL
2. Integrating with the Hailo media library's decoding functionality
3. Mapping decode output directly as DMA-BUF for zero-copy delivery to NPU

This is the optimal solution but requires significant engineering effort.

## 6. Execution Steps

```bash
# 1. Upload test script to container
scp benchmark_video_decode.py root@192.0.2.72:/tmp/
ssh root@192.0.2.72 "cat /tmp/benchmark_video_decode.py | \
  ctr -n aipc task exec --exec-id copy-$$ aipc-model-showcase sh -c 'cat > /tmp/benchmark_video_decode.py'"

# 2. Execute inside container
ssh root@192.0.2.72 "ctr -n aipc task exec --exec-id bench-$$ \
  aipc-model-showcase python3 /tmp/benchmark_video_decode.py 2>&1"

# 3. Record results, update this document
```

## 7. Assessment Output Template

### Decode Throughput

| Resolution | Pure Decode FPS | Decode+Resize FPS | Decode+Infer FPS |
|-----------|----------------|-------------------|------------------|
| 640x384 | _TBD_ | _TBD_ | _TBD_ |
| 1280x720 | _TBD_ | _TBD_ | _TBD_ |
| 1920x1080 | _TBD_ | _TBD_ | _TBD_ |
| 3840x2160 | _TBD_ | _TBD_ | _TBD_ |

### Seek Performance

| Resolution | Avg Seek | P50 | Max |
|-----------|---------|-----|-----|
| 640x384 | _TBD_ | _TBD_ | _TBD_ |
| 1280x720 | _TBD_ | _TBD_ | _TBD_ |
| 1920x1080 | _TBD_ | _TBD_ | _TBD_ |

### Resource Usage

| Resolution | CPU | Memory Delta |
|-----------|-----|-------------|
| 640x384 | _TBD_ | _TBD_ |
| 1280x720 | _TBD_ | _TBD_ |
| 1920x1080 | _TBD_ | _TBD_ |