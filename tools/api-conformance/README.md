# API Conformance Harness

Probes every operation in `docs/api/swagger.yaml` against a **live device** and
emits `report.json` + `report.md` (per-tag tables: op / tier / PASS-FAIL-SKIP /
actual HTTP status vs documented responses). Built to verify that the swagger
a remote caller reads matches what the device actually does.

## Usage

```bash
python3 run_conformance.py \
    --host 192.168.1.10 --username admin --password password \
    --swagger ./swagger.yaml        # repo copy, or scp the device's
    --report-dir ./reports          # ./reports/report.{json,md}
```

Dependencies: `pyyaml requests websockets` (plus stdlib). Exit code is non-zero
when any op FAILs, so it drops straight into CI or a regression loop.

Useful flags: `--only "METHOD /path-substring"` (debug one op),
`--exclude "METHOD /path,..."` (skip specific ops that destabilize an
unhealthy device — recorded as `SKIP-excluded` in the report, never silent),
`--skip-ws` (skip the events WebSocket probe), `--scheme http`
(device-local runs).

## Safety model (`classify.py`)

The device under test is real hardware — classification is conservative by
default and every tier is auditable:

| Tier | Behaviour | Examples |
|---|---|---|
| **S** safe | Execute fully; validate L1 (status documented) + L2 (envelope) | all GETs, `/events/publish`, `/ai/models/scan` |
| **W** reversible write | Execute with a self-cleaning body (self-created key / snapshot writeback / `/tmp` paths); clean up after | `POST /settings`, `POST /files/mkdir`, `PUT /device-info` |
| **D** destructive | **Never executed with a valid payload.** Gate probe only: a no-token call must get 401 | `/system/restart`, `/storage/format`, `POST /device/gpio`, `DELETE /event-logs` |
| **F** fixture | SKIP unless a binary fixture is supplied | `/apps` install, `/ai/models/upload`, `/files/upload` |

Physical-device writes (light/zoom/focus/GPIO/encoder config) are **D**, not W —
this is a shared lab machine. WebSocket endpoints are skipped at the HTTP layer
and probed via a real `wss://` handshake (`events/stream`).

## How it works

- `swagger_load.py` — parses the spec into ops (params, body schema,
  documented responses, tag).
- `classify.py` — tier per op: exact `W_OPS` overrides first, then `D`/`F`
  regex rules, then GET→S default.
- `discover.py` — calls list endpoints up front to fill `{model_id}`,
  `{app_id}`, `{stream_id}`, … with real instance IDs; missing instances
  become `SKIP-no-instance`, never fake IDs (filler `0` is used for D-tier
  gate paths only, where the request must not succeed).
- `probes.py` — one client session (login → Bearer); required bodies are
  synthesised from the schema when no explicit safe body exists; required
  query params from `QUERY_HINTS` or the schema.

## Reading a report

- **FAIL** = device behaviour contradicts the doc (undocumented status, broken
  envelope, missing 401 gate) — these are the findings that matter.
- **SKIP** is always annotated with a reason (`-fixture`, `-no-instance`,
  `-ws`, gate-only) — coverage holes are visible, not silent.

Known context for remote runs: the device serves the API at
`https://<host>/api/v1` behind a self-signed TLS gateway (the harness disables
cert verification), login is `POST /api/login` (**no** `/v1`), and the returned
token already carries the `Bearer ` prefix.
