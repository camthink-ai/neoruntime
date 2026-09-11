"""Endpoint safety classification for the conformance harness.

Tiers:
  S — safe: full probe (execute, validate L1/L2/L3)
  W — reversible write: execute with a self-cleaning/safe body, validate L1/L2
  D — destructive: never execute with valid payload. Only gate probes:
      (a) no-token call must be rejected 401
      (b) with-token call omitting required fields must be rejected 4xx
          (only when the op documents at least one required field; otherwise
          the with-token probe is skipped — an all-optional body might act)
  F — needs fixture (upload/package): SKIP-fixture unless a fixture is given

Tier lookup is by exact "METHOD /path-template" first, then by regex rules.
"""
import re

# --- exact overrides: W-tier op -> safe body (None = no body needed) ---------
# W is reserved for self-created/self-cleaned or snapshot-writeback writes.
# Physical-device and encoder-config writes are D below (shared lab device).
W_OPS = {
    "POST /settings": {"key": "__aipc_conformance_test__", "value": "1"},
    "DELETE /settings/{key}": None,  # deletes the key created above
    "POST /files/mkdir": {"path": "/tmp/aipc-conformance-dir"},
    # device files API enforces an allowed-roots whitelist (dataRoot + /tmp);
    # /etc/* is rejected 403 — use whitelisted real files instead.
    "POST /files/content": {"path": "/data/aipc/etc/swagger.yaml"},  # read-only
    "POST /files/rename": {"old_path": "/tmp/aipc-conformance-dir",
                           "new_path": "/tmp/aipc-conformance-dir2"},
    "DELETE /files": None,  # query param via QUERY_HINTS (impl reads query, not body)
    # Explicit real-world values (swagger's EventLogCreateRequest requires
    # event_type+source+message; synth_body would pass but with filler text).
    "POST /event-logs": {"event_type": "api.test", "level": "info",
                         "source": "api-conformance", "message": "conformance probe"},
    "PUT /device-info": None,    # read-modify-writeback current values
    "POST /files/batch-download": {"paths": ["/data/aipc/etc/swagger.yaml"]},
    "POST /debug-logs/export": {},
}

# Required-query-parameter values for S/W probes. Without these the probe
# omits documented required query params and the 400 it gets back is the
# harness's fault, not the device's.
QUERY_HINTS = {
    "GET /files/content": {"path": "/data/aipc/etc/swagger.yaml"},
    "GET /files/download": {"path": "/data/aipc/etc/swagger.yaml"},
    "DELETE /files": {"path": "/tmp/aipc-conformance-dir2"},  # self-created tmp
    "GET /logs/content": {"type": "service", "target": "platform-api",
                          "lines": 10},
    "GET /logs/download": {"type": "service", "target": "platform-api"},
    # example from the swagger itself; a real field path from GET /media/config
    "GET /media/config/field": {"field_path": "frontend.hailort.use-hailort-service"},
}

# D-tier: destructive. Never executed with a valid payload.
D_PATTERNS = [
    r"^POST /system/ota/install",
    r"^POST /system/os-upgrade/",
    r"^DELETE /system/os-upgrade/",
    r"^POST /storage/format",
    r"^POST /storage/mount",
    r"^POST /storage/unmount",
    r"^POST /processes/\{pid\}/kill",
    r"^POST /system/restart",
    r"^POST /system/password",
    r"^POST /system/time/set",
    r"^PUT /system/time/config",        # timezone write; the device rejects its
                                        # own GET value ("Universal") — write
                                        # path validated against a stricter
                                        # zone list than the read reports
    r"^POST /system/time/sync-from-client",  # sets system clock (200 verified
                                        # manually with an in-range timestamp)
    r"^PUT /system/time/timezone",
    r"^PUT /system/time/ntp",
    r"^POST /system/time/ntp/sync",
    r"^POST /network/config",
    r"^POST /ssh/config",
    r"^POST /files/batch-delete",
    r"^DELETE /ai/models/\{model_id\}",
    r"^DELETE /apps/\{app_id\}$",
    r"^DELETE /containers/\{id\}$",
    r"^DELETE /images/",
    r"^POST /images/pull",
    r"^POST /device/",   # ALL physical device writes (lens/fan/gpio/ptz/...)
    r"^PUT /device/",    # lens motor/limits writes
    r"^POST /device-info/factory",  # factory reset of device info
    r"^POST /containers/\{id\}/(start|stop|restart)",
    r"^POST /apps/\{app_id\}/(start|stop|restart)$",
    r"^PUT /store/installs/\{app_id\}",
    r"^DELETE /store/installs/\{app_id\}",
    r"^DELETE /dev/projects/\{id\}$",
    r"^PUT /dev/projects/\{id\}$",
    r"^POST /media/encoder/reconfig",  # heavy encoder reinit on live device
    r"^POST /media/pipeline/reconfigure",  # whole-pipeline reinit, same class
    r"^POST /media/config/import",     # whole-config replace
    r"^PUT /media/",                   # encoder/media config writes (reinit risk)
    r"^POST /media/streams$",          # creates a stream on a live device
    r"^DELETE /media/streams/",        # deletes a REAL stream config
    r"^POST /media/profile/switch",    # changes the active profile
    r"^PUT /audio/config",             # live audio config write
    r"^DELETE /event-logs$",           # purges logs older than N days (data loss)
]

# F-tier: needs a binary fixture / package to be meaningful.
F_PATTERNS = [
    r"^POST /apps$",
    r"^POST /apps/upload-image",
    r"^POST /apps/install-package",
    r"^POST /apps/wizard",
    r"^POST /store/",
    r"^POST /dev/projects",
    r"^POST /dev/projects/\{id\}/(upload|source|file|build)",
    r"^POST /ai/models/upload",
    r"^POST /files/upload",
    r"^POST /system/os-upgrade/upload",  # also D, but F wins (never executed)
]

_D_RE = [re.compile(p) for p in D_PATTERNS]
_F_RE = [re.compile(p) for p in F_PATTERNS]

# Session-terminal ops: executing one invalidates the token every later op
# needs. Never probed inline — run_conformance verifies them explicitly
# after everything else has finished.
TERMINAL_OPS = {"POST /logout"}


def classify(op):
    """Return (tier, safe_body) for an operation dict from swagger_load."""
    key = f"{op['method']} {op['path']}"
    if key in TERMINAL_OPS:
        return "L", None
    if key in W_OPS:
        return "W", W_OPS[key]
    for rx in _F_RE:
        if rx.search(key):
            return "F", None
    for rx in _D_RE:
        if rx.search(key):
            return "D", None
    if op["method"] == "GET":
        return "S", None
    # Remaining writes are safe-by-inspection endpoints
    # (register/scan/parse/publish etc. — validated against the device).
    return "S", None


# WebSocket endpoints (path -> tier)
WS_ENDPOINTS = {
    "/events/stream": "S",
    "/h264/main": "S",
    "/h264/sub": "S",
    "/terminal/ws": "D",
    "/containers/{id}/exec/ws": "D",
}
