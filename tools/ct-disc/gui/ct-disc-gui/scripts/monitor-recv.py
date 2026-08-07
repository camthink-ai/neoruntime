#!/usr/bin/env python3
"""
monitor-recv.py — Remote UDP collector for AIPC device monitor

Usage:  python3 monitor-recv.py [port] [output_dir]
        Default: port 9999, output ./monitor-remote/

Run on your WORKSTATION (not the device). The device runs monitor.sh with
a REMOTE argument pointing here. Each device's metrics land in its own CSV.
"""

import socket
import os
import sys
from datetime import datetime

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 9999
OUT_DIR = sys.argv[2] if len(sys.argv) > 2 else "./monitor-remote"

os.makedirs(OUT_DIR, exist_ok=True)

s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("0.0.0.0", PORT))

print(f"=== AIPC Monitor Receiver ===")
print(f"Listening on UDP :{PORT}")
print(f"Saving to: {OUT_DIR}/")
print(f"Press Ctrl+C to stop\n")

count = 0
try:
    while True:
        data, addr = s.recvfrom(4096)
        line = data.decode("utf-8", errors="replace").strip()
        ip = addr[0]
        ts = datetime.now().strftime("%Y%m%d_%H%M%S")

        # Alert lines → print immediately
        if "⚠" in line:
            print(f">>> ALERT [{ip}]: {line}")

        # Write to per-device file
        dev_file = os.path.join(OUT_DIR, f"{ip}.csv")
        with open(dev_file, "a") as f:
            f.write(line + "\n")

        count += 1
        if count % 60 == 0:
            print(f"[{ts}] received {count} rows from {ip}")

except KeyboardInterrupt:
    print(f"\n[{datetime.now():%Y%m%d_%H%M%S}] receiver stopped, {count} total rows")
    s.close()
