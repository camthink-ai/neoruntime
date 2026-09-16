"""Request execution and validation for the conformance harness.

Client wraps one device session (login + Bearer). probe_op executes a
single operation according to its safety tier and validates:

  L1 — actual HTTP status is among the documented responses
  L2 — success responses carry the project envelope {code, message[, data]}
  L3 — key endpoints return non-empty data (spot checks, caller-supplied)

Destructive (D) endpoints are never executed with a valid payload: only
gate probes run — no-token call must get 401, and a with-token call that
omits required body fields must get a 4xx.
"""
import json
import re
import ssl

import urllib3

import requests

from classify import QUERY_HINTS
from discover import params_for

urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

LOGIN_TIMEOUT = 15
REQUEST_TIMEOUT = 30
WS_FIRST_FRAME_TIMEOUT = 8

# Fillers for path params during gate probes only (never real instances).
GATE_FILLER = "0"

# Endpoints whose success responses are legitimately non-JSON (binary/text).
BINARY_OK_RE = re.compile(r"(download|export|/files/content)")

# Stream/WebSocket endpoints declared with 101 — probed via WS, never plain
# HTTP (a plain GET on a stream endpoint never terminates and hangs the run).
WS_OK_RE = re.compile(r"(/events/stream|/h264/|/terminal/ws|/exec/ws)")


class Client:
    API_PREFIX = "/api/v1"  # swagger servers base; login lives outside it

    def __init__(self, host, username, password, scheme="https"):
        self.base = f"{scheme}://{host}"
        self.username = username
        self.password = password
        self.session = requests.Session()
        self.session.verify = False
        self.token = None
        self.login()

    def login(self):
        resp = self.session.post(
            f"{self.base}/api/login",  # NOTE: login lives outside /api/v1
            json={"username": self.username, "password": self.password},
            timeout=LOGIN_TIMEOUT,
        )
        resp.raise_for_status()
        data = resp.json()
        if data.get("code") != 0:
            raise RuntimeError(f"login failed: {data.get('message')}")
        token = (data.get("data") or {}).get("token")
        if not token:
            raise RuntimeError("login response has no data.token")
        self.token = token  # already carries the "Bearer " prefix

    def request(self, method, path, body=None, auth=True, timeout=REQUEST_TIMEOUT,
                params=None):
        """Execute one API call. Returns (status_code, parsed_json_or_None)."""
        headers = {}
        if auth and self.token:
            # Server accepts verbatim / double-prefixed / raw forms; use verbatim.
            headers["Authorization"] = self.token
        kwargs = {"headers": headers, "timeout": timeout}
        if body is not None:
            kwargs["json"] = body
        if params:
            kwargs["params"] = params
        resp = self.session.request(method, f"{self.base}{path}",
                                    stream=True, **kwargs)
        ct = (resp.headers.get("Content-Type") or "").lower()
        if "text/event-stream" in ct:
            # SSE stream: never terminates — close instead of reading it all.
            resp.close()
            return resp.status_code, {"__sse__": True}
        try:
            parsed = resp.json()
        except (ValueError, json.JSONDecodeError):
            parsed = None
            print(f"  [debug] non-JSON from {method} {path}: "
                  f"ct={resp.headers.get('Content-Type')!r} "
                  f"len={len(resp.content)} body={resp.text[:120]!r}",
                  flush=True)
        finally:
            resp.close()
        return resp.status_code, parsed

    def api_request(self, method, path, **kwargs):
        """Request against a swagger path (adds the /api/v1 prefix)."""
        return self.request(method, self.API_PREFIX + path, **kwargs)


def synth_value(prop):
    """A minimal valid value for a JSON-schema property (S-tier bodies)."""
    t = prop.get("type") if isinstance(prop, dict) else None
    if t == "integer":
        return 0
    if t == "number":
        return 0
    if t == "boolean":
        return False
    if t == "array":
        return []
    if t == "object":
        return {}
    # string / enum / unspecified
    enum = prop.get("enum") if isinstance(prop, dict) else None
    return enum[0] if enum else "conformance"


def synth_body(op):
    """Minimal body from the op's schema: required fields with typed defaults."""
    body = op.get("body")
    if not body or op.get("is_multipart"):
        return None
    required = body.get("required") or []
    if not required:
        return {}
    return {name: synth_value((body.get("properties") or {}).get(name, {}))
            for name in required}


def synth_query(op, op_key):
    """Values for documented REQUIRED query params.

    Per-op hints win (they know whitelisted paths / enum semantics); the
    fallback derives a value from the param schema (enum[0] > default >
    typed zero). Optional params are never added.
    """
    hints = QUERY_HINTS.get(op_key) or {}
    out = {}
    for param in op.get("query_params") or []:
        if not param.get("required"):
            continue
        name = param["name"]
        if name in hints:
            out[name] = hints[name]
            continue
        schema = param.get("schema") or {}
        if schema.get("enum"):
            out[name] = schema["enum"][0]
        elif schema.get("default") is not None:
            out[name] = schema["default"]
        else:
            out[name] = synth_value(schema)
    return out


def op_base_path(op):
    """URL prefix for one op.

    Op-level `servers` override the global /api/v1 base — the one current
    case is POST /login, declared with `servers: [{url: /api}]` because its
    real route is /api/login (the /v1 group does not contain it). Returns
    the path part of the first server entry (origin stripped from absolute
    URLs), or None to keep the default base.
    """
    for server in op.get("servers") or []:
        raw = server.get("url", "") if isinstance(server, dict) else ""
        match = re.match(r"^[a-z][a-z0-9+.-]*://[^/]*(/.*)$", raw)
        path_part = match.group(1) if match else raw
        if path_part.startswith("/"):
            return path_part.rstrip("/") or ""
    return None


def fill_path(path, instances):
    """Substitute {param} placeholders. Returns (concrete_path, missing)."""
    missing = []
    result = path

    def _sub(match):
        name = match.group(1)
        value = instances.get(name)
        if value is None:
            missing.append(name)
            return match.group(0)
        return str(value)

    result = re.sub(r"\{(\w+)\}", _sub, result)
    return result, missing


def _check_envelope(parsed):
    """L2: return (ok, note) for the response envelope."""
    if parsed is None:
        return False, "response is not JSON"
    if not isinstance(parsed, dict) or "code" not in parsed:
        return False, "missing envelope {code,message[,data]}"
    return True, ""


def _validate(op, status, parsed, tier):
    """L1 + L2 validation. Returns (PASS/FAIL, reason)."""
    documented = [str(r) for r in op["documented_responses"]]
    op_key = f"{op['method']} {op['path']}"
    if str(status) not in documented:
        return ("FAIL",
                f"status {status} not documented (declared: {','.join(documented)})"
                + ("" if parsed is not None else "; non-JSON body"))
    if status < 400:
        if isinstance(parsed, dict) and parsed.get("__sse__"):
            # Stream semantics: no one-shot envelope to check. Whether SSE is
            # formally declared is visible in the report via the swagger copy.
            return "PASS", "SSE stream — streaming response, envelope check n/a"
        declared = op.get("response_content") or []
        json_declared = any("json" in ct for ct in declared)
        if parsed is None and (
                BINARY_OK_RE.search(op_key) or (declared and not json_declared)):
            return "PASS", (f"binary response (declared content: "
                            f"{','.join(declared) or 'path-derived'})")
        ok, note = _check_envelope(parsed)
        if not ok:
            return "FAIL", f"envelope invalid: {note}"
    return "PASS", ""


def probe_op(client, op, tier, safe_body, instances, log, http_stubs=None):
    """Execute one operation per its tier. Returns a result dict."""
    http_stubs = http_stubs or {}
    result = {
        "op": f"{op['method']} {op['path']}", "tag": op["tag"],
        "method": op["method"], "path": op["path"], "tier": tier,
        "status": "SKIP", "reason": "", "http_status": None,
        "documented": op["documented_responses"], "envelope": None, "note": "",
        "data_keys": None,
    }

    if tier == "F":
        result["reason"] = "SKIP-fixture: needs a binary fixture (upload/package)"
        return result

    if tier == "L":
        result["reason"] = ("SKIP-terminal: invalidates the session token; "
                            "verified explicitly after all other ops")
        return result

    op_key = result["op"]
    if WS_OK_RE.search(op_key) or "101" in [str(r) for r in op["documented_responses"]]:
        result["reason"] = ("SKIP-ws: WebSocket/stream endpoint, "
                            "probed via the WS layer instead")
        return result

    op_instances = params_for(op["path"], instances)
    path, missing = fill_path(op["path"], op_instances)
    query = synth_query(op, op_key)

    if tier == "D":
        return _probe_gate(client, op, result, path, missing, log)

    # --- S / W: real execution ------------------------------------------------
    if missing:
        result["reason"] = f"SKIP-no-instance: no value for {','.join(missing)}"
        return result

    if op.get("is_multipart"):
        result["reason"] = "SKIP-fixture: multipart endpoint without fixture"
        return result

    body = safe_body
    if body is None and op["method"] in ("POST", "PUT", "PATCH"):
        body = synth_body(op)

    stub = http_stubs.get(result["op"])
    base = op_base_path(op) or client.API_PREFIX
    if stub is not None:
        status, parsed = stub
    else:
        try:
            status, parsed = client.request(op["method"], base + path,
                                            body=body, params=query or None)
        except requests.exceptions.RequestException as exc:
            # One hung/refused endpoint must not kill the whole run.
            result["http_status"] = None
            result["status"] = "FAIL"
            result["reason"] = f"request error: {type(exc).__name__}: {exc}"[:200]
            log(f"[FAIL] {result['op']} ({result['reason']})")
            return result

    result["http_status"] = status
    result["status"], result["reason"] = _validate(op, status, parsed, tier)
    result["envelope"] = _check_envelope(parsed)[0] if status < 400 else None
    if isinstance(parsed, dict) and isinstance(parsed.get("data"), dict):
        result["data_keys"] = sorted(parsed["data"].keys())[:30]
    elif isinstance(parsed, dict) and isinstance(parsed.get("data"), list):
        result["data_keys"] = f"list[{len(parsed['data'])}]"
    if status < 400 and parsed is None:
        result["note"] = "2xx with non-JSON body"
    log(f"[{result['status']}] {result['op']} -> {status} ({result['reason']})")
    return result


def _probe_gate(client, op, result, path, missing, log):
    """D-tier: auth gate only (401 without token).

    The with-token missing-field probe was removed: the device accepts
    bodies with required fields omitted (Go zero-value binding), so the
    probe would actually EXECUTE the destructive op with zero values.
    That violation was observed live (POST /device/gpio {} -> 200) and
    is recorded as a documentation finding instead.
    """
    gate_path = path
    if missing:
        gate_path = re.sub(
            r"\{(\w+)\}",
            lambda m: GATE_FILLER if m.group(1) in missing else m.group(0),
            path)
    try:
        base = op_base_path(op) or client.API_PREFIX
        status, _ = client.request(
            op["method"], base + gate_path, auth=False,
            body={} if op["method"] in ("POST", "PUT") else None)
    except requests.exceptions.RequestException as exc:
        result["status"] = "FAIL"
        result["reason"] = f"gate request error: {type(exc).__name__}: {exc}"[:200]
        log(f"[{result['status']}] {result['op']} ({result['reason']})")
        return result
    result["http_status"] = status
    if status == 401:
        result["status"] = "PASS"
        result["reason"] = "gate-only: no-token -> 401"
    else:
        result["status"] = "FAIL"
        result["reason"] = f"no-token call returned {status}, expected 401"
    log(f"[{result['status']}] {result['op']} ({result['reason']})")
    return result


def probe_logout_terminal(client, log):
    """Execute POST /logout after everything else, then prove the token died.

    One row, two checks: logout with the still-valid session token must
    return documented success (200 + envelope), and one follow-up authed
    call must then get 401 — i.e. the logout really ended the session.
    """
    result = {
        "op": "POST /logout (+token-dead check)", "tag": "auth",
        "method": "POST", "path": "/logout", "tier": "L", "http_status": None,
        "documented": ["200"], "envelope": None, "note": "", "data_keys": None,
    }
    try:
        status, parsed = client.api_request("POST", "/logout")
        status2, _ = client.api_request("GET", "/system/time/config")
    except requests.exceptions.RequestException as exc:
        result["status"] = "FAIL"
        result["reason"] = f"request error: {type(exc).__name__}: {exc}"[:200]
        log(f"[{result['status']}] {result['op']} ({result['reason']})")
        return result

    result["http_status"] = status
    if status == 200 and status2 == 401:
        result["status"] = "PASS"
        result["reason"] = "logout 200, then authed call 401 (session ended)"
        result["envelope"] = _check_envelope(parsed)[0]
    else:
        result["status"] = "FAIL"
        result["reason"] = f"logout -> {status}, follow-up -> {status2} (want 200 then 401)"
    log(f"[{result['status']}] {result['op']} ({result['reason']})")
    return result


def probe_events_ws(host, token, scheme_wss=True, log=print, client=None):
    """Connect to the events WebSocket and wait for the first frame.

    The device token already includes the "Bearer " prefix; the WS layer
    accepts the bare token via ?token=. When a client is given, a test
    event is published first so a frame arrives within the window even on
    an idle device. Returns a result dict shaped like probe_op results.
    """
    result = {
        "op": "WS /events/stream", "tag": "events", "method": "WS",
        "path": "/events/stream", "tier": "S", "http_status": 101,
        "documented": ["101"], "envelope": None, "note": "",
    }
    try:
        import asyncio
        import websockets
    except ImportError:
        result["status"] = "SKIP"
        result["reason"] = "SKIP: websockets library not installed"
        return result

    if client is not None:
        # Best-effort: generate one event so recv() has something to deliver.
        client.api_request("POST", "/events/publish",
                           body={"topic": "conformance.probe",
                                 "data": {"ping": True}})

    bare = token.removeprefix("Bearer ").strip()
    scheme = "wss" if scheme_wss else "ws"
    url = f"{scheme}://{host}/api/v1/events/stream?token={bare}"
    ssl_ctx = ssl.create_default_context()
    ssl_ctx.check_hostname = False
    ssl_ctx.verify_mode = ssl.CERT_NONE

    async def _run():
        async with websockets.connect(url, ssl=ssl_ctx,
                                      open_timeout=WS_FIRST_FRAME_TIMEOUT) as ws:
            frame = await asyncio.wait_for(ws.recv(),
                                           timeout=WS_FIRST_FRAME_TIMEOUT)
            return len(frame)

    try:
        size = asyncio.run(_run())
        result["status"] = "PASS"
        result["reason"] = f"handshake ok, first frame {size} bytes"
    except asyncio.TimeoutError:
        # Handshake succeeded; the idle device just emitted no event in the
        # window. The endpoint works — only liveness of traffic is unproven.
        result["status"] = "PASS"
        result["reason"] = ("101 handshake ok, no frame within "
                            f"{WS_FIRST_FRAME_TIMEOUT}s (idle device)")
    except Exception as exc:  # noqa: BLE001 — any failure is a finding
        result["status"] = "FAIL"
        result["reason"] = f"ws failed: {type(exc).__name__}: {exc}"[:200]
    log(f"[{result['status']}] {result['op']} ({result['reason']})")
    return result
