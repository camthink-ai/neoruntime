#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Pack an OTA download image for boot/ota_module.

Output format (binary):
  ota_package_header_t (packed, little-endian) +
  optional padding (0x00) to app_offset alignment +
  app payload bytes (raw app .bin)

This matches ota_module_ota_download_start() / ota_module_ota_download_finish():
  - header_crc32 is CRC32 of header bytes excluding header_crc32 field
  - app_crc32 is CRC32 of app payload bytes (payload starts at app_offset)
"""

from __future__ import annotations

import argparse
import time
import struct
from pathlib import Path
from typing import Tuple


OTA_PACKAGE_MAGIC = 0x5441544F  # 'TAT O'? (as defined in ota_module.h)
OTA_PACKAGE_VERSION_MAX_LEN = 32
APP_OFFSET_ALIGN = 0x200  # bytes (VTOR requires 0x200 alignment)


def stm32_crc32_mpeg2(data: bytes) -> int:
    """
    STM32 HAL CRC32 (MPEG2 style):
      - Poly: 0x04C11DB7
      - Init: 0xFFFFFFFF
      - No reflection
      - No final XOR
    """
    poly = 0x04C11DB7
    crc = 0xFFFFFFFF
    for b in data:
        crc ^= (b << 24) & 0xFFFFFFFF
        for _ in range(8):
            if crc & 0x80000000:
                crc = ((crc << 1) ^ poly) & 0xFFFFFFFF
            else:
                crc = (crc << 1) & 0xFFFFFFFF
    return crc & 0xFFFFFFFF


def align_up(v: int, a: int) -> int:
    if a <= 0:
        return v
    return ((v + a - 1) // a) * a


def build_header(
    *,
    build_timestamp: int,
    app_offset: int,
    app_size: int,
    app_crc32: int,
    app_version: str,
) -> Tuple[bytes, int]:
    """
    Return (full_header_bytes, header_crc32).
    """
    version_bytes = app_version.encode("ascii", errors="ignore")[: (OTA_PACKAGE_VERSION_MAX_LEN - 1)]
    version_bytes = version_bytes + b"\x00" * (OTA_PACKAGE_VERSION_MAX_LEN - len(version_bytes))

    # Packed struct (ota_module.h):
    # uint32 magic, uint32 build_timestamp, uint32 app_offset,
    # uint32 app_size, uint32 app_crc32, char app_version[32], uint32 header_crc32
    header_wo_crc = struct.pack(
        "<IIIII32s",
        OTA_PACKAGE_MAGIC,
        build_timestamp,
        app_offset,
        app_size,
        app_crc32,
        version_bytes,
    )

    header_crc32 = stm32_crc32_mpeg2(header_wo_crc)

    header = header_wo_crc + struct.pack("<I", header_crc32)
    return header, header_crc32


def extract_version_from_sys_config(repo_root: Path) -> str:
    """
    Parse APP_VERSION from app/Custom/User/sys_config.h (format: #define APP_VERSION "x.y.z").
    """
    sys_cfg = repo_root / "app" / "Custom" / "User" / "sys_config.h"
    if not sys_cfg.exists():
        return "app"
    text = sys_cfg.read_text(encoding="utf-8", errors="ignore")
    for line in text.splitlines():
        line = line.strip()
        if line.startswith("#define") and "APP_VERSION" in line:
            parts = line.split()
            if len(parts) >= 3 and parts[1] == "APP_VERSION":
                raw = parts[2].strip()
                if raw.startswith('"') and raw.endswith('"') and len(raw) >= 2:
                    return raw[1:-1]
    return "app"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--app-bin", required=True, help="Input app .bin (raw payload)")
    ap.add_argument("--out", required=True, help="Output ota package .bin")
    ap.add_argument("--version", default=None, help="app_version string (ASCII, max 31 chars). Default: APP_VERSION in app/Custom/User/sys_config.h")
    ap.add_argument("--timestamp", type=int, default=None, help="build_timestamp (unix seconds). Default: now")
    args = ap.parse_args()

    app_path = Path(args.app_bin)
    if not app_path.exists():
        raise FileNotFoundError(f"app-bin not found: {app_path}")
    app_data = app_path.read_bytes()

    build_timestamp = int(time.time()) if args.timestamp is None else int(args.timestamp)

    repo_root = Path(__file__).resolve().parents[1]
    if args.version is None:
        app_version = extract_version_from_sys_config(repo_root)
    else:
        app_version = args.version

    # app_offset points to start of app payload inside the OTA package.
    # ota_package_header_t size = 56 bytes (6x uint32 + 32 bytes version)
    header_size = 56
    app_offset = align_up(header_size, APP_OFFSET_ALIGN)
    pad_len = app_offset - header_size

    app_size = len(app_data)
    app_crc32 = stm32_crc32_mpeg2(app_data)

    header, header_crc32 = build_header(
        build_timestamp=build_timestamp,
        app_offset=app_offset,
        app_size=app_size,
        app_crc32=app_crc32,
        app_version=app_version,
    )

    base_out = Path(args.out)
    if not base_out.is_absolute():
        # keep consistent with pack_to_hex.py: treat relative as under repo root/build/
        base_out = repo_root / base_out
    # Append version suffix before extension: ne503_ota_package_v<version>.bin
    stem = base_out.stem
    suffix = base_out.suffix or ".bin"
    safe_ver = app_version.replace(" ", "_")
    out_path = base_out.with_name(f"{stem}_v{safe_ver}{suffix}")
    out_path.parent.mkdir(parents=True, exist_ok=True)

    out_path.write_bytes(header + (b"\x00" * pad_len) + app_data)

    print(f"[OK] OTA package written: {out_path}")
    print(f"  header_size = {len(header)} bytes")
    print(f"  pad_len     = {pad_len} bytes")
    print(f"  app_offset  = {app_offset:#x}")
    print(f"  app_size    = {app_size} bytes")
    print(f"  app_crc32   = 0x{app_crc32:08X}")
    print(f"  header_crc32= 0x{header_crc32:08X}")
    print(f"  version     = {app_version!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

