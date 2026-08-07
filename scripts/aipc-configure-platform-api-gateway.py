#!/usr/bin/env python3
import argparse
import re
import sys
from pathlib import Path


MODES = {
    "nginx": {
        "http_addr": '"127.0.0.1:8080"',
        "tls_enabled": "false",
        "redirect_http": "false",
    },
    "direct": {
        "http_addr": '":8080"',
        "tls_enabled": "true",
        "redirect_http": "true",
    },
}


KEY_RE = re.compile(r"^(\s*)([A-Za-z_][A-Za-z0-9_-]*):(.*)$")


def render_line(indent, key, value, suffix, has_newline):
    newline = "\n" if has_newline else ""
    comment = ""
    body = suffix
    if "#" in body:
        comment = body[body.index("#") :]
    if comment:
        return f"{indent}{key}: {value} {comment.rstrip()}{newline}"
    return f"{indent}{key}: {value}{newline}"


def update_config(path, mode):
    values = MODES[mode]
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines(keepends=True)
    output = []
    service_indent = None
    tls_indent = None
    changed = set()

    for line in lines:
        match = KEY_RE.match(line)
        if not match:
            output.append(line)
            continue

        indent, key, suffix = match.groups()
        has_newline = line.endswith("\n")
        current_indent = len(indent)

        if service_indent is not None and current_indent <= service_indent and key != "service":
            service_indent = None
            tls_indent = None
        if tls_indent is not None and current_indent <= tls_indent and key != "tls":
            tls_indent = None

        if current_indent == 0 and key == "service":
            service_indent = current_indent
            tls_indent = None
            output.append(line)
            continue

        if service_indent is not None and current_indent > service_indent and key == "tls":
            tls_indent = current_indent
            output.append(line)
            continue

        if service_indent is not None and current_indent > service_indent and key == "http_addr":
            output.append(render_line(indent, key, values["http_addr"], suffix, has_newline))
            changed.add("http_addr")
            continue

        if (
            service_indent is not None
            and tls_indent is not None
            and current_indent > tls_indent
            and key == "enabled"
        ):
            output.append(render_line(indent, key, values["tls_enabled"], suffix, has_newline))
            changed.add("tls.enabled")
            continue

        if (
            service_indent is not None
            and tls_indent is not None
            and current_indent > tls_indent
            and key == "redirect_http"
        ):
            output.append(render_line(indent, key, values["redirect_http"], suffix, has_newline))
            changed.add("tls.redirect_http")
            continue

        output.append(line)

    required = {"http_addr", "tls.enabled", "tls.redirect_http"}
    missing = sorted(required - changed)
    if missing:
        raise ValueError("missing platform-api service keys: %s" % ", ".join(missing))

    path.write_text("".join(output), encoding="utf-8")
    return changed


def main():
    parser = argparse.ArgumentParser(description="Switch platform-api between direct TLS and nginx gateway mode.")
    parser.add_argument("--config", default="/data/aipc/etc/platform-api.yaml", type=Path)
    parser.add_argument("--mode", choices=sorted(MODES), required=True)
    args = parser.parse_args()

    update_config(args.config, args.mode)
    print("platform-api gateway mode: %s (%s)" % (args.mode, args.config))
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print("aipc-configure-platform-api-gateway: %s" % exc, file=sys.stderr)
        sys.exit(1)
