#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Generate ONE combined Intel HEX for STM32G0:
  - boot bin at 0x08000000
  - OTA area erased (filled with 0xFF) for [record .. app1)
  - OTA package bin at 0x08010000 (slot base; contains header + padding + app payload)

The OTA layout is taken from boot/ota_module/inc/ota_module_config.h:
  OTA_PLATFORM_RECORD_ADDR = 0x08000000 + 56K = 0x0800E000
  OTA_PLATFORM_RECORD_SIZE = 4K
  OTA_PLATFORM_RESERVED_SIZE = 4K
  OTA_PLATFORM_APP1_ADDR = 0x08010000

So OTA erase region = [0x0800E000 .. 0x08010000) = 8K.
"""

from __future__ import annotations

import argparse
import sys
from datetime import datetime
from pathlib import Path
from typing import Optional, Tuple, List

# Reuse version parsing from pack_ota.py (same APP_VERSION source as OTA package naming)
_TOOLS_DIR = Path(__file__).resolve().parent
if str(_TOOLS_DIR) not in sys.path:
    sys.path.insert(0, str(_TOOLS_DIR))
from pack_ota import extract_version_from_sys_config  # noqa: E402


def calculate_checksum(data: bytes) -> int:
    return (0x100 - (sum(data) & 0xFF)) & 0xFF


def write_extended_linear_address(hex_file, address: int) -> None:
    high_addr = (address >> 16) & 0xFFFF
    record = bytes([0x02, 0x00, 0x00, 0x04, (high_addr >> 8) & 0xFF, high_addr & 0xFF])
    checksum = calculate_checksum(record)
    hex_file.write(f":{record.hex().upper()}{checksum:02X}\n")


def write_data_record(hex_file, address: int, data: bytes, current_high_addr_ref: List[Optional[int]]) -> None:
    offset = 0
    while offset < len(data):
        chunk_size = min(16, len(data) - offset)
        chunk = data[offset:offset + chunk_size]
        current_addr = address + offset
        current_high = (current_addr >> 16) & 0xFFFF
        low_addr = current_addr & 0xFFFF

        if current_high_addr_ref[0] != current_high:
            write_extended_linear_address(hex_file, current_addr)
            current_high_addr_ref[0] = current_high

        record = bytes([chunk_size, (low_addr >> 8) & 0xFF, low_addr & 0xFF, 0x00]) + chunk
        checksum = calculate_checksum(record)
        hex_file.write(f":{record.hex().upper()}{checksum:02X}\n")
        offset += chunk_size


def write_end_of_file(hex_file) -> None:
    record = bytes([0x00, 0x00, 0x00, 0x01])
    checksum = calculate_checksum(record)
    hex_file.write(f":{record.hex().upper()}{checksum:02X}\n")


def read_required(path: Path) -> bytes:
    if not path.exists():
        raise FileNotFoundError(f"Required file not found: {path}")
    return path.read_bytes()


def _safe_filename_component(s: str) -> str:
    """Keep version strings filesystem-friendly on Windows/Linux."""
    out = []
    for c in s:
        if c.isalnum() or c in ".-_":
            out.append(c)
        else:
            out.append("_")
    return "".join(out) or "unknown"


def combined_hex_output_path(base: Path, repo_root: Path) -> Path:
    """
    Turn e.g. build/ne503_Main.hex into build/ne503_Main_v0.1.2_20260513.hex
    (version from app/Custom/User/sys_config.h, date = local YYYYMMDD).
    """
    ver = _safe_filename_component(extract_version_from_sys_config(repo_root))
    date_str = datetime.now().strftime("%Y%m%d")
    return base.with_name(f"{base.stem}_v{ver}_{date_str}{base.suffix}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", required=True, help="Output .hex path")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parents[1]
    build_dir = repo_root / "build"

    # Inputs (produced by our Makefiles and copied to root build/)
    boot_bin = build_dir / "ne503_mcu_boot.bin"
    # Prefer OTA package produced by tools/pack_ota.py
    # (pack_ota appends _v<version> to the output filename)
    ota_candidates = sorted(build_dir.glob("ne503_ota_package*_v*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not ota_candidates:
        ota_candidates = sorted(build_dir.glob("ne503_ota_package*.bin"), key=lambda p: p.stat().st_mtime, reverse=True)
    if not ota_candidates:
        raise FileNotFoundError(
            f"OTA package not found in {build_dir}. Build it with tools/pack_ota.py first."
        )
    ota_pkg_bin = ota_candidates[0]

    # Addresses (match linker scripts + ota_module_config.h)
    BOOT_ADDR = 0x08000000
    OTA_ERASE_ADDR = 0x0800E000
    OTA_ERASE_SIZE = 0x2000  # 8KB => [0x0800E000..0x08010000)
    APP_ADDR = 0x08010000

    boot_data = read_required(boot_bin)
    app_data = read_required(ota_pkg_bin)
    ota_erase = bytes([0xFF]) * OTA_ERASE_SIZE

    items = [
        ("BOOT", BOOT_ADDR, boot_data),
        ("OTA_ERASE", OTA_ERASE_ADDR, ota_erase),
        ("APP", APP_ADDR, app_data),
    ]
    items.sort(key=lambda x: x[1])

    out_path = Path(args.output)
    # Root Makefile passes build/ne503_Main.hex; do NOT double-prefix with build/.
    if not out_path.is_absolute():
        out_path = repo_root / out_path
    out_path = combined_hex_output_path(out_path, repo_root)
    out_path.parent.mkdir(parents=True, exist_ok=True)

    with out_path.open("w", encoding="ascii") as f:
        current_high_addr: List[Optional[int]] = [None]
        for _name, addr, data in items:
            write_data_record(f, addr, data, current_high_addr)
        write_end_of_file(f)

    print(f"[OK] Wrote combined HEX: {out_path}")
    print(f"  BOOT @ 0x{BOOT_ADDR:08X}, {len(boot_data)} bytes")
    print(f"  OTA  @ 0x{OTA_ERASE_ADDR:08X}, {len(ota_erase)} bytes (0xFF)")
    print(f"  APP  @ 0x{APP_ADDR:08X}, {len(app_data)} bytes (ota package: {ota_pkg_bin.name})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

