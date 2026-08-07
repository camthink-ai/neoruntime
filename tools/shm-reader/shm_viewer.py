#!/usr/bin/env python3
"""
SHM Video Viewer - View live video from camera-daemon SHM ring buffer.
Zero mandatory dependencies (pure Python3). Optional: numpy, cv2, Pillow, ffmpeg.

Modes:
  --http [PORT]     MJPEG/BMP stream over HTTP (default, open in browser)
  --jpeg [DIR]      Save frames as JPEG/BMP files
  --display         OpenCV window (requires cv2)

Usage:
  python3 shm_viewer.py /run/aipc/shm/stream0.raw --http 8080
  python3 shm_viewer.py /run/aipc/shm/stream0.raw --jpeg /tmp/frames -n 20
"""

import argparse
import mmap
import os
import signal
import struct
import subprocess
import sys
import time
from http.server import HTTPServer, BaseHTTPRequestHandler
from pathlib import Path
import threading
import io
import json

# ========== SHM Protocol ==========

SHM_MAGIC = 0x41495043
SHM_HEADER_SIZE = 4096
SHM_SLOT_HDR_SIZE = 64
SHM_MAX_PLANES = 3
SHM_SLOT_READY = 2

HEADER_FMT = "<IIIIIII3IIIIIQQ"
HEADER_FIELDS = [
    "magic", "version", "width", "height", "format", "fps", "num_planes",
    "stride0", "stride1", "stride2",
    "buffer_count", "slot_size", "data_offset", "_pad0",
    "write_seq", "latest_slot",
]

SLOT_HDR_FMT = "<QQII II II 3I3I"
SLOT_HDR_FIELDS = [
    "sequence", "timestamp_ns", "data_size", "state",
    "width", "height", "format", "num_planes",
    "plane_offset0", "plane_offset1", "plane_offset2",
    "plane_size0", "plane_size1", "plane_size2",
]


class ShmReader:
    def __init__(self, path):
        self.path = path
        self.fd = os.open(path, os.O_RDONLY)
        size = os.fstat(self.fd).st_size
        if size < SHM_HEADER_SIZE:
            raise ValueError(f"File too small: {size}")
        self.mm = mmap.mmap(self.fd, size, access=mmap.ACCESS_READ)
        self._parse_header()
        self._last_seq = self._read_write_seq()

    def _parse_header(self):
        raw = struct.unpack_from(HEADER_FMT, self.mm, 0)
        self.hdr = dict(zip(HEADER_FIELDS, raw))
        if self.hdr["magic"] != SHM_MAGIC:
            raise ValueError(f"Bad magic: 0x{self.hdr['magic']:08X}")

    def _read_write_seq(self):
        return struct.unpack_from("<Q", self.mm, 0x38)[0]

    def _read_latest_slot(self):
        return struct.unpack_from("<Q", self.mm, 0x40)[0]

    def _read_slot(self, idx):
        off = SHM_HEADER_SIZE + idx * self.hdr["slot_size"]
        raw = struct.unpack_from(SLOT_HDR_FMT, self.mm, off)
        return dict(zip(SLOT_HDR_FIELDS, raw))

    def _read_slot_data(self, idx, data_size):
        off = SHM_HEADER_SIZE + idx * self.hdr["slot_size"] + SHM_SLOT_HDR_SIZE
        return self.mm[off: off + data_size]

    def poll_frame(self, timeout_ms=200):
        deadline = time.monotonic() + timeout_ms / 1000.0
        while time.monotonic() < deadline:
            seq = self._read_write_seq()
            if seq != self._last_seq:
                slot_idx = self._read_latest_slot()
                if slot_idx < self.hdr["buffer_count"]:
                    slot = self._read_slot(slot_idx)
                    if slot["state"] == SHM_SLOT_READY:
                        data = self._read_slot_data(slot_idx, slot["data_size"])
                        self._last_seq = seq
                        return slot, data
                self._last_seq = seq
            time.sleep(0.001)
        return None, None

    def info(self):
        return self.hdr

    def close(self):
        self.mm.close()
        os.close(self.fd)


# ========== Image Conversion Backends ==========

def _find_nv12_to_jpeg():
    """Find the nv12-to-jpeg binary (co-located or in PATH)."""
    candidates = [
        os.path.join(os.path.dirname(os.path.abspath(__file__)), "nv12-to-jpeg"),
        "/opt/aipc/bin/nv12-to-jpeg",
    ]
    for c in candidates:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    # Check PATH
    try:
        r = subprocess.run(["which", "nv12-to-jpeg"], capture_output=True, timeout=2)
        if r.returncode == 0:
            return r.stdout.decode().strip()
    except Exception:
        pass
    return None


def _detect_backend():
    """Detect best available conversion backend."""
    # 1. nv12-to-jpeg native binary (fastest, zero-dep)
    if _find_nv12_to_jpeg():
        return "native"

    # 2. numpy + cv2
    try:
        import numpy as np
        import cv2
        return "cv2"
    except ImportError:
        pass

    # 3. numpy + Pillow
    try:
        import numpy as np
        from PIL import Image
        return "pillow"
    except ImportError:
        pass

    # 4. ffmpeg subprocess
    try:
        r = subprocess.run(["ffmpeg", "-version"], capture_output=True, timeout=3)
        if r.returncode == 0:
            return "ffmpeg"
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    # 5. Pure Python (slowest, always available)
    return "pure"


class ImageConverter:
    """Convert NV12 raw data to viewable image format."""

    def __init__(self, backend=None, width=0, height=0, quality=85):
        self.backend = backend or _detect_backend()
        self._native_proc = None
        self._native_w = 0
        self._native_h = 0
        self._native_q = 0
        print(f"Image backend: {self.backend}")

    def nv12_to_image(self, data, width, height, quality=85):
        """
        Convert NV12 data to encoded image bytes.
        Returns (image_bytes, content_type).
        """
        if self.backend == "native":
            return self._native(data, width, height, quality)
        elif self.backend == "cv2":
            return self._cv2(data, width, height, quality)
        elif self.backend == "pillow":
            return self._pillow(data, width, height, quality)
        elif self.backend == "ffmpeg":
            return self._ffmpeg(data, width, height, quality)
        else:
            return self._pure_bmp(data, width, height)

    def _native(self, data, width, height, quality):
        """Use co-deployed nv12-to-jpeg binary in --pipe mode."""
        # (Re)start pipe process if resolution/quality changed
        if (self._native_proc is None or self._native_proc.poll() is not None
                or self._native_w != width or self._native_h != height
                or self._native_q != quality):
            if self._native_proc and self._native_proc.poll() is None:
                self._native_proc.kill()
                self._native_proc.wait()
            bin_path = _find_nv12_to_jpeg()
            self._native_proc = subprocess.Popen(
                [bin_path, "--pipe", "-w", str(width), "-h", str(height),
                 "-q", str(quality)],
                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                bufsize=0,
            )
            self._native_w = width
            self._native_h = height
            self._native_q = quality

        frame_size = width * height * 3 // 2
        raw = data[:frame_size]
        if len(raw) < frame_size:
            raw = raw + b'\x80' * (frame_size - len(raw))

        try:
            self._native_proc.stdin.write(raw)
            self._native_proc.stdin.flush()
            # Read 4-byte length prefix
            hdr = b''
            while len(hdr) < 4:
                chunk = self._native_proc.stdout.read(4 - len(hdr))
                if not chunk:
                    raise IOError("native process died")
                hdr += chunk
            jpeg_len = struct.unpack('<I', hdr)[0]
            jpeg = b''
            while len(jpeg) < jpeg_len:
                chunk = self._native_proc.stdout.read(jpeg_len - len(jpeg))
                if not chunk:
                    raise IOError("native process died")
                jpeg += chunk
            return jpeg, "image/jpeg"
        except (IOError, BrokenPipeError) as e:
            print(f"  native pipe error: {e}, restarting...", file=sys.stderr)
            self._native_proc = None
            return self._pure_bmp(data, width, height)

    def close(self):
        if self._native_proc and self._native_proc.poll() is None:
            self._native_proc.kill()
            self._native_proc.wait()

    def _cv2(self, data, width, height, quality):
        import numpy as np
        import cv2
        y_size = width * height
        uv_size = width * (height // 2)
        total = y_size + uv_size
        raw = data[:total] if len(data) >= total else data + b'\x80' * (total - len(data))
        yuv = np.frombuffer(raw, dtype=np.uint8).reshape(height * 3 // 2, width)
        bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)
        _, buf = cv2.imencode('.jpg', bgr, [cv2.IMWRITE_JPEG_QUALITY, quality])
        return buf.tobytes(), "image/jpeg"

    def _pillow(self, data, width, height, quality):
        import numpy as np
        from PIL import Image
        rgb = self._nv12_to_rgb_np(data, width, height)
        img = Image.fromarray(rgb)
        buf = io.BytesIO()
        img.save(buf, format='JPEG', quality=quality)
        return buf.getvalue(), "image/jpeg"

    def _nv12_to_rgb_np(self, data, width, height):
        import numpy as np
        y_size = width * height
        uv_size = width * (height // 2)
        total = y_size + uv_size
        raw = data[:total] if len(data) >= total else data + b'\x80' * (total - len(data))
        y = np.frombuffer(raw[:y_size], dtype=np.uint8).reshape(height, width).astype(np.float32)
        uv = np.frombuffer(raw[y_size:total], dtype=np.uint8).reshape(height // 2, width)
        u = uv[:, 0::2].repeat(2, axis=0).repeat(2, axis=1).astype(np.float32) - 128.0
        v = uv[:, 1::2].repeat(2, axis=0).repeat(2, axis=1).astype(np.float32) - 128.0
        r = np.clip(y + 1.402 * v, 0, 255).astype(np.uint8)
        g = np.clip(y - 0.344136 * u - 0.714136 * v, 0, 255).astype(np.uint8)
        b = np.clip(y + 1.772 * u, 0, 255).astype(np.uint8)
        return np.stack([r, g, b], axis=-1)

    def _ffmpeg(self, data, width, height, quality):
        """Use ffmpeg to convert NV12 → JPEG."""
        try:
            proc = subprocess.run(
                [
                    "ffmpeg", "-y",
                    "-f", "rawvideo",
                    "-pix_fmt", "nv12",
                    "-s", f"{width}x{height}",
                    "-i", "pipe:0",
                    "-frames:v", "1",
                    "-q:v", str(max(1, min(31, (100 - quality) * 31 // 100))),
                    "-f", "image2pipe",
                    "-vcodec", "mjpeg",
                    "pipe:1",
                ],
                input=data[:width * height * 3 // 2],
                capture_output=True,
                timeout=5,
            )
            if proc.returncode == 0 and len(proc.stdout) > 0:
                return proc.stdout, "image/jpeg"
        except Exception as e:
            print(f"  ffmpeg error: {e}", file=sys.stderr)

        # Fallback to BMP
        return self._pure_bmp(data, width, height)

    def _pure_bmp(self, data, width, height):
        """
        Pure Python NV12 → BMP. No dependencies.
        Downscales 2x if resolution > 960 for performance.
        """
        # Downscale for large frames (pure Python is slow)
        scale = 1
        while width // scale > 960:
            scale *= 2

        out_w = width // scale
        out_h = height // scale

        y_size = width * height
        uv_offset = y_size
        expected = y_size + width * (height // 2)

        if len(data) < expected:
            data = data + b'\x80' * (expected - len(data))

        # BMP is bottom-up, BGR, row padded to 4 bytes
        row_pad = (4 - (out_w * 3) % 4) % 4
        bmp_row_size = out_w * 3 + row_pad
        pixel_size = bmp_row_size * out_h

        # BMP header (14) + DIB header (40)
        bmp_size = 54 + pixel_size
        bmp = bytearray(bmp_size)

        # BMP file header
        bmp[0:2] = b'BM'
        struct.pack_into('<I', bmp, 2, bmp_size)
        struct.pack_into('<I', bmp, 10, 54)
        # DIB header
        struct.pack_into('<I', bmp, 14, 40)
        struct.pack_into('<i', bmp, 18, out_w)
        struct.pack_into('<i', bmp, 22, out_h)  # positive = bottom-up
        struct.pack_into('<H', bmp, 26, 1)      # planes
        struct.pack_into('<H', bmp, 28, 24)     # bits per pixel
        struct.pack_into('<I', bmp, 34, pixel_size)

        # Pixel conversion (NV12 → BGR, bottom-up)
        for oy in range(out_h):
            bmp_row = out_h - 1 - oy  # BMP bottom-up
            sy = oy * scale
            for ox in range(out_w):
                sx = ox * scale
                y_val = data[sy * width + sx]
                uv_row = (sy // 2)
                uv_col = (sx // 2) * 2
                u_val = data[uv_offset + uv_row * width + uv_col] - 128
                v_val = data[uv_offset + uv_row * width + uv_col + 1] - 128

                r = max(0, min(255, int(y_val + 1.402 * v_val)))
                g = max(0, min(255, int(y_val - 0.344136 * u_val - 0.714136 * v_val)))
                b = max(0, min(255, int(y_val + 1.772 * u_val)))

                off = 54 + bmp_row * bmp_row_size + ox * 3
                bmp[off] = b
                bmp[off + 1] = g
                bmp[off + 2] = r

        return bytes(bmp), "image/bmp"


# ========== HTTP MJPEG Server ==========

def run_http(reader, port, quality):
    hdr = reader.info()
    converter = ImageConverter()

    print(f"Stream: {hdr['width']}x{hdr['height']} @ {hdr['fps']}fps")
    print(f"Open in browser: http://0.0.0.0:{port}/")
    print(f"  /stream    - live video")
    print(f"  /snapshot  - single frame")
    print(f"  /info      - JSON metadata")
    print()

    latest_img = [None]
    latest_ct = ["image/jpeg"]
    latest_lock = threading.Lock()
    running = [True]
    frame_count = [0]

    def frame_producer():
        while running[0]:
            slot, data = reader.poll_frame(timeout_ms=200)
            if slot is None:
                continue
            try:
                w = slot["width"] or hdr["width"]
                h = slot["height"] or hdr["height"]
                img, ct = converter.nv12_to_image(data, w, h, quality)
                with latest_lock:
                    latest_img[0] = img
                    latest_ct[0] = ct
                frame_count[0] += 1
                if frame_count[0] == 1:
                    print(f"  First frame converted ({len(img)} bytes, {ct})")
            except Exception as e:
                print(f"  Convert error: {e}", file=sys.stderr)

    # Determine boundary content type
    def get_stream_ct():
        with latest_lock:
            return latest_ct[0]

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            if self.path == "/stream":
                self.send_response(200)
                self.send_header("Content-Type",
                    "multipart/x-mixed-replace; boundary=frame")
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Access-Control-Allow-Origin", "*")
                self.end_headers()
                try:
                    while running[0]:
                        with latest_lock:
                            img = latest_img[0]
                            ct = latest_ct[0]
                        if img is None:
                            time.sleep(0.05)
                            continue
                        self.wfile.write(b"--frame\r\n")
                        self.wfile.write(f"Content-Type: {ct}\r\n".encode())
                        self.wfile.write(f"Content-Length: {len(img)}\r\n".encode())
                        self.wfile.write(b"\r\n")
                        self.wfile.write(img)
                        self.wfile.write(b"\r\n")
                        self.wfile.flush()
                        time.sleep(1.0 / max(hdr["fps"], 1))
                except (BrokenPipeError, ConnectionResetError):
                    pass

            elif self.path == "/snapshot":
                with latest_lock:
                    img = latest_img[0]
                    ct = latest_ct[0]
                if img:
                    self.send_response(200)
                    self.send_header("Content-Type", ct)
                    self.send_header("Content-Length", str(len(img)))
                    self.end_headers()
                    self.wfile.write(img)
                else:
                    self.send_response(503)
                    self.end_headers()
                    self.wfile.write(b"No frame yet")

            elif self.path == "/info":
                body = json.dumps({
                    "width": hdr["width"], "height": hdr["height"],
                    "fps": hdr["fps"], "format": hdr["format"],
                    "buffers": hdr["buffer_count"],
                    "backend": converter.backend,
                    "frames": frame_count[0],
                }, indent=2).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            elif self.path == "/":
                html = f"""<!DOCTYPE html>
<html><head><title>AIPC SHM Viewer</title>
<style>
  body {{ background:#1a1a1a; color:#eee; font-family:monospace;
         display:flex; flex-direction:column; align-items:center; padding:20px }}
  img {{ max-width:95vw; max-height:80vh; border:2px solid #444 }}
  h1 {{ color:#4fc3f7 }}
  .info {{ color:#aaa; margin:10px 0 }}
  a {{ color:#4fc3f7 }}
</style>
<script>
  // Auto-refresh snapshot if multipart stream not supported
  let img;
  window.onload = function() {{
    img = document.getElementById('live');
    img.onerror = function() {{
      // Fallback: refresh snapshot every 500ms
      img.src = '/snapshot?' + Date.now();
      setInterval(function() {{ img.src = '/snapshot?' + Date.now(); }}, 500);
    }};
  }};
</script>
</head><body>
<h1>AIPC Camera Stream</h1>
<div class="info">{hdr['width']}x{hdr['height']} @ {hdr['fps']}fps | backend: {converter.backend}</div>
<img id="live" src="/stream" alt="Live Stream" />
<div class="info" style="margin-top:15px">
  <a href="/snapshot">Snapshot</a> | <a href="/info">JSON</a>
</div>
</body></html>"""
                body = html.encode()
                self.send_response(200)
                self.send_header("Content-Type", "text/html")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            else:
                self.send_response(404)
                self.end_headers()

    producer = threading.Thread(target=frame_producer, daemon=True)
    producer.start()

    server = HTTPServer(("0.0.0.0", port), Handler)

    def stop(*_):
        running[0] = False
        threading.Thread(target=server.shutdown, daemon=True).start()
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        running[0] = False
        print("\nStopped.")


# ========== JPEG/BMP File Dump ==========

def run_jpeg(reader, out_dir, max_frames, quality):
    hdr = reader.info()
    converter = ImageConverter()
    os.makedirs(out_dir, exist_ok=True)
    print(f"Saving up to {max_frames} frames to {out_dir}/ ...")

    count = 0
    running = [True]
    signal.signal(signal.SIGINT, lambda *_: running.__setitem__(0, False))

    while running[0] and count < max_frames:
        slot, data = reader.poll_frame(timeout_ms=500)
        if slot is None:
            continue
        w = slot["width"] or hdr["width"]
        h = slot["height"] or hdr["height"]
        img, ct = converter.nv12_to_image(data, w, h, quality)
        ext = "jpg" if "jpeg" in ct else "bmp"
        path = os.path.join(out_dir, f"frame_{count:06d}_{w}x{h}.{ext}")
        with open(path, "wb") as f:
            f.write(img)
        print(f"  [{count}] seq={slot['sequence']} -> {path} ({len(img)} bytes)")
        count += 1

    print(f"Saved {count} frames")


# ========== OpenCV Display ==========

def run_display(reader):
    import cv2
    import numpy as np
    hdr = reader.info()
    win = f"AIPC ({hdr['width']}x{hdr['height']})"
    cv2.namedWindow(win, cv2.WINDOW_NORMAL)
    print(f"Press 'q' to quit.")
    while True:
        slot, data = reader.poll_frame(timeout_ms=100)
        if slot is not None:
            w = slot["width"] or hdr["width"]
            h = slot["height"] or hdr["height"]
            total = w * h * 3 // 2
            if len(data) >= total:
                yuv = np.frombuffer(data[:total], dtype=np.uint8).reshape(h * 3 // 2, w)
                bgr = cv2.cvtColor(yuv, cv2.COLOR_YUV2BGR_NV12)
                cv2.imshow(win, bgr)
        if cv2.waitKey(1) & 0xFF == ord('q'):
            break
    cv2.destroyAllWindows()


# ========== Main ==========

def main():
    p = argparse.ArgumentParser(description="View camera-daemon SHM video")
    p.add_argument("shm_file", help="SHM file (e.g. /run/aipc/shm/stream0.raw)")
    g = p.add_mutually_exclusive_group()
    g.add_argument("--http", type=int, nargs="?", const=8080, metavar="PORT",
                   help="HTTP MJPEG stream (default port 8080)")
    g.add_argument("--jpeg", type=str, nargs="?", const=".", metavar="DIR",
                   help="Save as JPEG/BMP files")
    g.add_argument("--display", action="store_true", help="OpenCV window")
    p.add_argument("-n", "--count", type=int, default=30, help="Max frames for --jpeg")
    p.add_argument("-q", "--quality", type=int, default=85, help="JPEG quality 1-100")
    p.add_argument("--backend", choices=["cv2", "pillow", "ffmpeg", "pure"],
                   help="Force conversion backend")
    args = p.parse_args()

    reader = ShmReader(args.shm_file)
    hdr = reader.info()
    print(f"SHM: {hdr['width']}x{hdr['height']} @ {hdr['fps']}fps, "
          f"{hdr['buffer_count']} buffers, v{hdr['version']}")

    try:
        if args.display:
            run_display(reader)
        elif args.jpeg is not None:
            run_jpeg(reader, args.jpeg, args.count, args.quality)
        else:
            port = args.http if args.http else 8080
            run_http(reader, port, args.quality)
    finally:
        reader.close()


if __name__ == "__main__":
    main()
