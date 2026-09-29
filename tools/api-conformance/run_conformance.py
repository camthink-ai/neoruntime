#!/usr/bin/env python3
"""API conformance runner: probe every operation in a swagger file against
a live device, safely, and emit report.json + report.md.

Usage:
  python3 run_conformance.py \
      --host 192.168.1.10 --username admin --password password \
      --swagger ./swagger.yaml --report-dir ./reports

Safety model (see classify.py): S full probe, W reversible write with a
self-cleaning body, D gate-only (never executed), F fixture skip.
"""
import argparse
import json
import sys
import time
from collections import Counter, defaultdict
from pathlib import Path

from classify import W_OPS, classify
from discover import discover_all, writeback_snapshot
from probes import Client, probe_events_ws, probe_logout_terminal, probe_op
from swagger_load import load_operations

TEST_SETTINGS_KEY = "__aipc_conformance_test__"

# W-tier ops whose body is a snapshot writeback instead of a fixed literal.
WRITEBACK_SNAPSHOT = {
    "PUT /device-info": "device_info",
}


def build_parser():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", required=True)
    parser.add_argument("--username", default="admin")
    parser.add_argument("--password", default="password")
    parser.add_argument("--scheme", default="https", choices=["https", "http"])
    parser.add_argument("--swagger", required=True,
                        help="path to swagger.yaml (device copy or repo copy)")
    parser.add_argument("--report-dir", default="./reports")
    parser.add_argument("--skip-ws", action="store_true",
                        help="skip the events WebSocket probe")
    parser.add_argument("--only", default=None,
                        help="substring filter on 'METHOD /path' (debugging)")
    parser.add_argument("--exclude", default=None,
                        help="comma-separated exact 'METHOD /path' ops to skip "
                             "entirely (e.g. an op that hangs an unhealthy "
                             "device); reported as SKIP-excluded, never silent")
    parser.add_argument("--exclude-reason", default="excluded by operator",
                        help="reason string recorded for --exclude ops")
    parser.add_argument("--request-delay", type=float, default=0.0,
                        help="seconds to pause between operations; small "
                             "values (0.2-0.5) keep watchdog-equipped "
                             "devices from hard-resetting under sweep load")
    return parser


def resolve_body(op_key, tier, safe_body, snapshots):
    """W-tier writeback ops get their snapshot as the body."""
    if tier == "W" and op_key in WRITEBACK_SNAPSHOT:
        snapshot = snapshots.get(WRITEBACK_SNAPSHOT[op_key])
        if isinstance(snapshot, dict) and snapshot:
            return snapshot
    return safe_body


def cleanup(client, log):
    """Best-effort removal of W-tier leftovers (in case a probe failed)."""
    try:
        client.request("DELETE", "/api/v1/settings/" + TEST_SETTINGS_KEY)
    except Exception as exc:  # noqa: BLE001
        log(f"cleanup settings key: {exc}")
    try:
        # DELETE /files reads the path from the query string, not the body.
        client.request("DELETE", "/api/v1/files",
                       params={"path": "/tmp/aipc-conformance-dir2"})
    except Exception as exc:  # noqa: BLE001
        log(f"cleanup tmp dir: {exc}")


def write_reports(report_dir, meta, results):
    out = Path(report_dir)
    out.mkdir(parents=True, exist_ok=True)

    by_tier = Counter(r["tier"] for r in results)
    by_status = Counter(r["status"] for r in results)
    summary = {
        "total": len(results),
        "pass": by_status.get("PASS", 0),
        "fail": by_status.get("FAIL", 0),
        "skip": by_status.get("SKIP", 0),
        "by_tier": dict(by_tier),
    }
    payload = {"meta": meta, "summary": summary, "results": results}
    (out / "report.json").write_text(
        json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")

    lines = [
        "# API Conformance Report",
        "",
        f"- host: `{meta['host']}`",
        f"- swagger: `{meta['swagger']}` (api {meta['api_version']}, "
        f"{meta['path_count']} paths)",
        f"- time: {meta['timestamp']}",
        f"- **{summary['pass']} PASS / {summary['fail']} FAIL / "
        f"{summary['skip']} SKIP** of {summary['total']}",
        "",
    ]
    by_tag = defaultdict(list)
    for r in results:
        by_tag[r["tag"]].append(r)
    for tag in sorted(by_tag):
        rows = by_tag[tag]
        stat = Counter(r["status"] for r in rows)
        lines += [f"## {tag} "
                  f"({stat.get('PASS', 0)}P/{stat.get('FAIL', 0)}F/"
                  f"{stat.get('SKIP', 0)}S)", "",
                  "| op | tier | status | http | documented | reason |",
                  "|---|---|---|---|---|---|"]
        for r in sorted(rows, key=lambda x: x["op"]):
            http = r["http_status"] if r["http_status"] is not None else "-"
            doc = ",".join(str(d) for d in r["documented"]) or "-"
            reason = (r["reason"] or r["note"] or "").replace("|", "\\|")
            lines.append(f"| `{r['op']}` | {r['tier']} | {r['status']} | "
                         f"{http} | {doc} | {reason} |")
        lines.append("")
    (out / "report.md").write_text("\n".join(lines), encoding="utf-8")
    return summary


def main():
    args = build_parser().parse_args()

    def log(msg):
        print(msg, flush=True)

    catalog = load_operations(args.swagger)
    operations = catalog["operations"]
    if args.only:
        operations = [o for o in operations
                      if args.only in f"{o['method']} {o['path']}"]
    log(f"swagger: {len(catalog['operations'])} operations "
        f"(api {catalog['info'].get('version')}), testing {len(operations)}")

    client = Client(args.host, args.username, args.password, scheme=args.scheme)
    log(f"login ok (token {client.token[:12]}...)")

    def get_json(path):
        try:
            status, parsed = client.request("GET", "/api/v1" + path)
            return parsed if isinstance(parsed, dict) else None
        except Exception:  # noqa: BLE001
            return None

    snapshots = writeback_snapshot(get_json)
    instances = discover_all(get_json, log)
    # The W-tier DELETE must target the self-created key, never a real one.
    # (Per-template buckets: settings' key never reaches /store/apps/{key}.)
    instances.setdefault("/settings", {})["key"] = TEST_SETTINGS_KEY

    results = []
    excluded = {s.strip() for s in (args.exclude or "").split(",") if s.strip()}
    for op in operations:
        op_key = f"{op['method']} {op['path']}"
        if op_key in excluded:
            log(f"[SKIP] {op_key} (excluded: {args.exclude_reason})")
            results.append({
                "op": op_key, "tag": op["tag"], "method": op["method"],
                "path": op["path"], "tier": "X", "status": "SKIP",
                "reason": f"SKIP-excluded: {args.exclude_reason}",
                "http_status": None,
                "documented": op["documented_responses"], "envelope": None,
                "note": "", "data_keys": None,
            })
            continue
        tier, safe_body = classify(op)
        if tier == "W" and op_key in WRITEBACK_SNAPSHOT:
            # A snapshot writeback without its snapshot would synth an empty
            # body and risk zeroing the resource — skip instead.
            snap = snapshots.get(WRITEBACK_SNAPSHOT[op_key])
            if not (isinstance(snap, dict) and snap):
                log(f"[SKIP] {op_key} (no snapshot to write back)")
                results.append({
                    "op": op_key, "tag": op["tag"], "method": op["method"],
                    "path": op["path"], "tier": tier, "status": "SKIP",
                    "reason": "SKIP-no-snapshot: read-back failed, will not "
                              "write a synthesized body", "http_status": None,
                    "documented": op["documented_responses"], "envelope": None,
                    "note": "", "data_keys": None,
                })
                continue
        body = resolve_body(op_key, tier, safe_body, snapshots)
        results.append(probe_op(client, op, tier, body, instances, log))
        if args.request_delay > 0:
            time.sleep(args.request_delay)

    if not args.skip_ws:
        results.append(probe_events_ws(args.host, client.token, log=log,
                                       client=client))

    cleanup(client, log)
    # Session-terminal op, last by definition: logout must succeed AND kill
    # the token. Nothing may use the client after this point.
    results.append(probe_logout_terminal(client, log))

    meta = {
        "host": args.host,
        "swagger": str(args.swagger),
        "api_version": catalog["info"].get("version"),
        "path_count": len(catalog["spec"].get("paths") or {}),
        "timestamp": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    summary = write_reports(args.report_dir, meta, results)
    log(f"report: {Path(args.report_dir).resolve()}/report.md "
        f"({summary['pass']}P/{summary['fail']}F/{summary['skip']}S)")
    sys.exit(1 if summary["fail"] else 0)


if __name__ == "__main__":
    main()
