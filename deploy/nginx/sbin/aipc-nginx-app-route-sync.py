#!/usr/bin/env python3
import argparse
import json
import os
import re
import signal
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path
from urllib.parse import quote


DEFAULT_REGISTRY_PATH = Path("/data/aipc/apps/registry/registry.json")
DEFAULT_MANIFESTS_DIR = Path("/data/aipc/apps/manifests")
DEFAULT_OUTPUT_PATH = Path("/data/nginx/conf/app-routes.conf")
DEFAULT_ROOT_UPSTREAMS_PATH = Path("/data/nginx/conf/app-root-upstreams.conf")
DEFAULT_ROOT_PREFIXES_PATH = Path("/data/nginx/conf/app-root-prefixes.conf")
DEFAULT_NGINX_BIN = "/data/nginx/bin/nginx"
DEFAULT_NGINX_PREFIX = "/data/nginx/"
DEFAULT_NGINX_CONF = "conf/nginx.conf"
DEFAULT_NGINX_LIB = (
    "/data/nginx/rootfs/lib/aarch64-linux-gnu:"
    "/data/nginx/rootfs/usr/lib/aarch64-linux-gnu"
)
DEFAULT_CERT_PATH = Path("/data/aipc/etc/ssl/server.crt")
DEFAULT_KEY_PATH = Path("/data/aipc/etc/ssl/server.key")

APP_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,127}$")
STOP_REQUESTED = False


def log(message):
    print(time.strftime("%Y-%m-%d %H:%M:%S"), message, flush=True)


def safe_scalar(value):
    value = value.strip()
    if "#" in value:
        value = value.split("#", 1)[0].strip()
    if len(value) >= 2 and value[0] == value[-1] and value[0] in ("'", '"'):
        return value[1:-1]
    return value


def indent_of(line):
    return len(line) - len(line.lstrip(" "))


def parse_manifest_network(manifest_path):
    mode = None
    inbound = []
    try:
        lines = Path(manifest_path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError as exc:
        return None, [], "manifest_read_error: %s" % exc

    for index, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if not re.match(r"^\s*network\s*:\s*(#.*)?$", line):
            continue

        network_indent = indent_of(line)
        inbound_indent = None
        in_inbound = False
        cursor = index + 1
        while cursor < len(lines):
            raw = lines[cursor]
            current = raw.strip()
            if not current or current.startswith("#"):
                cursor += 1
                continue

            current_indent = indent_of(raw)
            if current_indent <= network_indent:
                break

            if in_inbound and current_indent <= inbound_indent:
                in_inbound = False

            if in_inbound:
                match = re.match(r"^\s*-\s*([0-9]{1,5})\s*(#.*)?$", raw)
                if match:
                    inbound.append(int(match.group(1)))
                cursor += 1
                continue

            mode_match = re.match(r"^\s*mode\s*:\s*(.+)$", raw)
            if mode_match:
                mode = safe_scalar(mode_match.group(1))
                cursor += 1
                continue

            inbound_match = re.match(r"^\s*inbound\s*:\s*(.*)$", raw)
            if inbound_match:
                inbound_indent = current_indent
                inline = safe_scalar(inbound_match.group(1))
                if inline.startswith("[") and inline.endswith("]"):
                    inbound.extend(int(port) for port in re.findall(r"[0-9]{1,5}", inline))
                in_inbound = True
                cursor += 1
                continue

            cursor += 1

        if mode or inbound:
            break

    return mode, inbound, None


def normalize_web_path(value):
    if not isinstance(value, str) or not value.strip():
        return "/"
    path = value.strip()
    if "\n" in path or "\r" in path:
        return "/"
    if "://" in path:
        path = "/" + path.split("://", 1)[1].split("/", 1)[-1]
    if not path.startswith("/"):
        path = "/" + path
    return quote(path, safe="/:?&=+,%._~-")


def route_redirect_path(app_id, web_url):
    path = normalize_web_path(web_url)
    if path == "/":
        return "/apps/%s/" % app_id
    return "/apps/%s%s" % (app_id, path)


def valid_port(value):
    try:
        port = int(value)
    except (TypeError, ValueError):
        return None
    if 1 <= port <= 65535:
        return port
    return None


def load_registry(registry_path):
    if not registry_path.exists():
        return {}
    with registry_path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    if not isinstance(data, dict):
        raise ValueError("registry root is not an object")
    return data


def discover_routes(registry_path):
    registry = load_registry(registry_path)
    routes = []
    skipped = []

    for registry_id, app in sorted(registry.items()):
        if not isinstance(app, dict):
            skipped.append((str(registry_id), "registry entry is not an object"))
            continue
        app_id = str(app.get("id") or registry_id)
        if not APP_ID_RE.match(app_id):
            skipped.append((app_id, "invalid app id"))
            continue
        if app.get("state") != "running":
            continue
        web_url = app.get("web_url")
        if not web_url:
            continue

        manifest_path = app.get("manifest_path")
        if not manifest_path:
            skipped.append((app_id, "missing manifest_path"))
            continue

        mode, inbound, error = parse_manifest_network(manifest_path)
        if error:
            skipped.append((app_id, error))
            continue
        if mode != "host":
            skipped.append((app_id, "network.mode is not host"))
            continue
        port = valid_port(inbound[0] if inbound else None)
        if port is None:
            skipped.append((app_id, "missing valid inbound port"))
            continue

        routes.append({
            "id": app_id,
            "name": str(app.get("name") or app_id).replace("\n", " "),
            "port": port,
            "web_url": web_url,
        })

    return routes, skipped


def render_routes(registry_path):
    routes, skipped = discover_routes(registry_path)
    lines = [
        "# Generated by /data/nginx/sbin/aipc-nginx-app-route-sync.py. Do not edit.",
        "# Source: %s" % registry_path,
        "# Updated: %s" % time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "",
    ]

    if not routes:
        lines.append("# No running app web routes found.")
        lines.append("")

    for route in routes:
        app_id = route["id"]
        port = route["port"]
        redirect_path = route_redirect_path(app_id, route["web_url"])
        lines.extend([
            "# %s -> 127.0.0.1:%s" % (route["name"], port),
            "location = /apps/%s {" % app_id,
            "    return 302 %s;" % redirect_path,
            "}",
            "",
            "location ^~ /apps/%s/ {" % app_id,
            "    proxy_pass http://127.0.0.1:%s/;" % port,
            "    proxy_set_header Host $host;",
            "    proxy_set_header X-Real-IP $remote_addr;",
            "    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;",
            "    proxy_set_header X-Forwarded-Proto https;",
            "    proxy_set_header X-Forwarded-Host $host;",
            "    proxy_set_header X-Forwarded-Port 443;",
            "    proxy_set_header X-Forwarded-Prefix /apps/%s;" % app_id,
            "    proxy_set_header Upgrade $http_upgrade;",
            "    proxy_set_header Connection $connection_upgrade;",
            "    proxy_http_version 1.1;",
            "    proxy_read_timeout 300s;",
            "    proxy_buffering off;",
            "    proxy_set_header Accept-Encoding \"\";",
            "    sub_filter_once off;",
            "    sub_filter_types text/css application/javascript text/javascript;",
            "    sub_filter 'href=\"/' 'href=\"/apps/%s/';" % app_id,
            "    sub_filter \"href='/\" \"href='/apps/%s/\";" % app_id,
            "    sub_filter 'src=\"/' 'src=\"/apps/%s/';" % app_id,
            "    sub_filter \"src='/\" \"src='/apps/%s/\";" % app_id,
            "    sub_filter 'action=\"/' 'action=\"/apps/%s/';" % app_id,
            "    sub_filter \"action='/\" \"action='/apps/%s/\";" % app_id,
            "    sub_filter 'url(/' 'url(/apps/%s/';" % app_id,
            "    proxy_redirect ~^(/.*)$ /apps/%s$1;" % app_id,
            "    proxy_redirect http://127.0.0.1:%s/ /apps/%s/;" % (port, app_id),
            "    proxy_cookie_path / /apps/%s/;" % app_id,
            "}",
            "",
        ])

    if skipped:
        lines.append("# Skipped apps:")
        for app_id, reason in skipped:
            reason = str(reason).replace("\n", " ")
            lines.append("# - %s: %s" % (app_id, reason))
        lines.append("")

    return "\n".join(lines), routes


def render_root_upstreams(registry_path, routes):
    lines = [
        "# Generated by /data/nginx/sbin/aipc-nginx-app-route-sync.py. Do not edit.",
        "# Source: %s" % registry_path,
        "# Updated: %s" % time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "",
    ]
    if not routes:
        lines.append("# No running app root-path upstreams found.")
        lines.append("")
        return "\n".join(lines)

    for route in routes:
        app_id = route["id"]
        lines.append(
            "~^https?://[^/]+/apps/%s(?:/|$) http://127.0.0.1:%s;"
            % (re.escape(app_id), route["port"])
        )
    lines.append("")
    return "\n".join(lines)


def render_root_prefixes(registry_path, routes):
    lines = [
        "# Generated by /data/nginx/sbin/aipc-nginx-app-route-sync.py. Do not edit.",
        "# Source: %s" % registry_path,
        "# Updated: %s" % time.strftime("%Y-%m-%dT%H:%M:%S%z"),
        "",
    ]
    if not routes:
        lines.append("# No running app referer prefixes found.")
        lines.append("")
        return "\n".join(lines)

    for route in routes:
        app_id = route["id"]
        lines.append("~^https?://[^/]+/apps/%s(?:/|$) /apps/%s;" % (re.escape(app_id), app_id))
    lines.append("")
    return "\n".join(lines)


def nginx_env(nginx_lib):
    env = os.environ.copy()
    old = env.get("LD_LIBRARY_PATH")
    env["LD_LIBRARY_PATH"] = nginx_lib if not old else nginx_lib + ":" + old
    return env


def nginx_command(args, nginx_bin, nginx_prefix, nginx_conf):
    return [nginx_bin, "-p", nginx_prefix, "-c", nginx_conf] + args


def run_nginx(args, nginx_bin, nginx_prefix, nginx_conf, nginx_lib):
    return subprocess.run(
        nginx_command(args, nginx_bin, nginx_prefix, nginx_conf),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        env=nginx_env(nginx_lib),
    )


def collect_device_ips():
    ips = ["127.0.0.1"]

    try:
        for item in socket.gethostbyname_ex(socket.gethostname())[2]:
            if re.match(r"^\d+\.\d+\.\d+\.\d+$", item):
                ips.append(item)
    except OSError:
        pass

    for command in (["hostname", "-I"], ["ip", "-o", "-4", "addr", "show"]):
        try:
            result = subprocess.run(
                command,
                stdout=subprocess.PIPE,
                stderr=subprocess.DEVNULL,
                text=True,
                timeout=2,
            )
        except (OSError, subprocess.SubprocessError):
            continue
        if result.returncode != 0:
            continue
        for item in re.findall(r"\b(?:\d{1,3}\.){3}\d{1,3}\b", result.stdout):
            if all(0 <= int(part) <= 255 for part in item.split(".")):
                ips.append(item)

    seen = set()
    unique = []
    for ip in ips:
        if ip in seen:
            continue
        seen.add(ip)
        unique.append(ip)
    return unique


def ensure_tls_certificate(cert_path, key_path):
    if cert_path.exists() and cert_path.stat().st_size > 0 and key_path.exists() and key_path.stat().st_size > 0:
        return False

    cert_path.parent.mkdir(parents=True, exist_ok=True)
    os.chmod(cert_path.parent, 0o700)

    ips = collect_device_ips()
    alt_names = ["DNS.1 = localhost"]
    for index, ip in enumerate(ips, start=1):
        alt_names.append("IP.%s = %s" % (index, ip))

    config = """[req]
prompt = no
distinguished_name = req_distinguished_name
x509_extensions = v3_req

[req_distinguished_name]
CN = AIPC Device

[v3_req]
basicConstraints = critical,CA:TRUE
keyUsage = digitalSignature,keyEncipherment,keyCertSign
extendedKeyUsage = serverAuth
subjectAltName = @alt_names

[alt_names]
%s
""" % "\n".join(alt_names)

    with tempfile.TemporaryDirectory(prefix=".aipc-nginx-cert.", dir=str(cert_path.parent)) as tmpdir:
        tmpdir_path = Path(tmpdir)
        config_path = tmpdir_path / "openssl.cnf"
        tmp_cert = tmpdir_path / "server.crt"
        tmp_key = tmpdir_path / "server.key"
        config_path.write_text(config, encoding="utf-8")
        result = subprocess.run(
            [
                "openssl",
                "req",
                "-x509",
                "-nodes",
                "-newkey",
                "rsa:2048",
                "-sha256",
                "-days",
                "3650",
                "-config",
                str(config_path),
                "-keyout",
                str(tmp_key),
                "-out",
                str(tmp_cert),
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
        )
        if result.returncode != 0:
            raise RuntimeError("failed to generate nginx TLS certificate:\n%s" % result.stdout.strip())
        os.chmod(tmp_key, 0o600)
        os.chmod(tmp_cert, 0o644)
        os.replace(str(tmp_key), str(key_path))
        os.replace(str(tmp_cert), str(cert_path))

    log("generated TLS certificate for nginx at %s" % cert_path)
    return True


def nginx_test(args):
    result = run_nginx(
        ["-t"],
        args.nginx_bin,
        args.nginx_prefix,
        args.nginx_conf,
        args.nginx_lib,
    )
    if result.returncode != 0:
        raise RuntimeError("nginx -t failed:\n%s" % result.stdout.strip())
    return result.stdout.strip()


def nginx_reload(args):
    result = run_nginx(
        ["-s", "reload"],
        args.nginx_bin,
        args.nginx_prefix,
        args.nginx_conf,
        args.nginx_lib,
    )
    if result.returncode != 0:
        raise RuntimeError("nginx reload failed:\n%s" % result.stdout.strip())
    return result.stdout.strip()


def write_candidates(candidates, args):
    old_files = {}
    changed = False
    for path, content in candidates.items():
        path.parent.mkdir(parents=True, exist_ok=True)
        old = path.read_bytes() if path.exists() else None
        old_files[path] = old
        old_text = old.decode("utf-8", errors="replace") if old is not None else None
        if old_text != content:
            changed = True

    if not changed:
        return False

    for path, content in candidates.items():
        old_text = (
            old_files[path].decode("utf-8", errors="replace")
            if old_files[path] is not None
            else None
        )
        if old_text == content:
            continue
        tmp_path = path.with_name(".%s.tmp.%s" % (path.name, os.getpid()))
        tmp_path.write_text(content, encoding="utf-8")
        os.replace(str(tmp_path), str(path))

    try:
        nginx_test(args)
    except Exception:
        for path, old in old_files.items():
            if old is None:
                try:
                    path.unlink()
                except FileNotFoundError:
                    pass
            else:
                restore = path.with_name(".%s.restore.%s" % (path.name, os.getpid()))
                restore.write_bytes(old)
                os.replace(str(restore), str(path))
        raise
    return True


def sync_once(args, reload_nginx):
    if args.ensure_cert:
        ensure_tls_certificate(args.cert_path, args.key_path)

    content, routes = render_routes(args.registry_path)
    root_upstreams = render_root_upstreams(args.registry_path, routes)
    root_prefixes = render_root_prefixes(args.registry_path, routes)
    changed = write_candidates({
        args.output_path: content,
        args.root_upstreams_path: root_upstreams,
        args.root_prefixes_path: root_prefixes,
    }, args)
    if changed:
        log("generated %s route(s): %s" % (len(routes), ", ".join(r["id"] for r in routes) or "none"))
        if reload_nginx:
            nginx_reload(args)
            log("nginx reloaded")
    return changed


def stat_signature(path):
    try:
        stat = Path(path).stat()
        return (str(path), stat.st_mtime_ns, stat.st_size)
    except OSError:
        return (str(path), None, None)


def watch_snapshot(args):
    paths = {args.registry_path, args.manifests_dir}
    try:
        registry = load_registry(args.registry_path)
        for app in registry.values():
            if isinstance(app, dict) and app.get("manifest_path"):
                paths.add(Path(app["manifest_path"]))
    except Exception:
        pass
    return tuple(sorted(stat_signature(path) for path in paths))


def watch_loop(args, nginx_proc=None):
    log("watching app registry and manifests")
    last = None
    while not STOP_REQUESTED:
        if nginx_proc is not None and nginx_proc.poll() is not None:
            raise RuntimeError("nginx exited with status %s" % nginx_proc.returncode)
        current = watch_snapshot(args)
        if current != last:
            if last is not None and args.debounce > 0:
                time.sleep(args.debounce)
            try:
                sync_once(args, args.reload)
            except Exception as exc:
                log("sync failed: %s" % exc)
            last = watch_snapshot(args)
        time.sleep(args.interval)


def install_signal_handlers():
    def request_stop(_signum, _frame):
        global STOP_REQUESTED
        STOP_REQUESTED = True

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)


def start_nginx_foreground(args):
    nginx_test(args)
    command = nginx_command(["-g", "daemon off;"], args.nginx_bin, args.nginx_prefix, args.nginx_conf)
    proc = subprocess.Popen(command, env=nginx_env(args.nginx_lib))
    log("nginx started in foreground, pid=%s" % proc.pid)
    return proc


def stop_nginx(args, proc):
    try:
        nginx_reload_args = ["-s", "quit"]
        run_nginx(
            nginx_reload_args,
            args.nginx_bin,
            args.nginx_prefix,
            args.nginx_conf,
            args.nginx_lib,
        )
    except Exception as exc:
        log("nginx quit signal failed: %s" % exc)
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.terminate()
        try:
            proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
    log("nginx stopped")


def serve(args):
    install_signal_handlers()
    sync_once(args, reload_nginx=False)
    proc = start_nginx_foreground(args)
    args.reload = True
    try:
        watch_loop(args, nginx_proc=proc)
    finally:
        stop_nginx(args, proc)
    return 0


def build_parser():
    parser = argparse.ArgumentParser(description="Generate nginx app proxy routes from AIPC app registry.")
    parser.add_argument("--watch", action="store_true", help="watch registry and manifests continuously")
    parser.add_argument("--serve", action="store_true", help="run nginx in foreground and watch app routes")
    parser.add_argument("--interval", type=float, default=2.0, help="polling interval in seconds")
    parser.add_argument("--debounce", type=float, default=0.5, help="debounce delay in seconds")
    parser.add_argument("--reload", action="store_true", help="reload nginx after route changes")
    parser.add_argument("--ensure-cert", action="store_true", help="create the nginx TLS certificate if absent")
    parser.add_argument("--cert-path", type=Path, default=DEFAULT_CERT_PATH)
    parser.add_argument("--key-path", type=Path, default=DEFAULT_KEY_PATH)
    parser.add_argument("--registry-path", type=Path, default=DEFAULT_REGISTRY_PATH)
    parser.add_argument("--manifests-dir", type=Path, default=DEFAULT_MANIFESTS_DIR)
    parser.add_argument("--output-path", type=Path, default=DEFAULT_OUTPUT_PATH)
    parser.add_argument("--root-upstreams-path", type=Path, default=DEFAULT_ROOT_UPSTREAMS_PATH)
    parser.add_argument("--root-prefixes-path", type=Path, default=DEFAULT_ROOT_PREFIXES_PATH)
    parser.add_argument("--nginx-bin", default=DEFAULT_NGINX_BIN)
    parser.add_argument("--nginx-prefix", default=DEFAULT_NGINX_PREFIX)
    parser.add_argument("--nginx-conf", default=DEFAULT_NGINX_CONF)
    parser.add_argument("--nginx-lib", default=DEFAULT_NGINX_LIB)
    return parser


def main():
    args = build_parser().parse_args()

    if args.serve:
        return serve(args)
    if args.watch:
        args.reload = True
        watch_loop(args)
        return 0

    changed = sync_once(args, args.reload)
    if not changed:
        log("routes already up to date")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        log("fatal: %s" % exc)
        sys.exit(1)
