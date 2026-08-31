#!/usr/bin/env python3
"""CI gate: docs/api/swagger.yaml must document exactly the routes
registered by platform/platform-api/server/main.go.

Prints both set differences and exits 1 when either is non-empty, so a
route that lands without its spec entry (or a spec entry whose route was
removed) fails CI instead of silently drifting.

Run from anywhere; paths are resolved relative to the repo root.
"""
import os
import re
import sys

import yaml

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAIN_GO = os.path.join(ROOT, "platform", "platform-api", "server", "main.go")
SWAGGER = os.path.join(ROOT, "docs", "api", "swagger.yaml")
BASE = "/api/v1"
HTTP = ("get", "post", "put", "delete", "patch", "head", "options")


def registered_routes():
    src = open(MAIN_GO, encoding="utf-8").read()
    # strip line comments first: a disabled (commented-out) registration is
    # not a live route and must not keep a stale spec entry green
    src = "\n".join(line.split("//", 1)[0] for line in src.splitlines())

    # registration forms this parser does not understand must fail loudly
    # instead of silently passing as "in sync" (Any/Handle are gin's generic
    # registration APIs; narrower matches like HandlerFunc/HandleWebSocket
    # are not registrations)
    unknown = {m.group(1) for m in re.finditer(r"\b\w+\.(Any|Handle)\(", src)}
    if unknown:
        sys.exit(
            f"unparsed gin registration form(s) {sorted(unknown)} in {MAIN_GO} — "
            "extend this parser to cover them"
        )

    groups = {}
    for m in re.finditer(r'(\w+)\s*:?=\s*([\w.]+)\.Group\("([^"]*)"\)', src):
        var, parent, prefix = m.groups()
        if parent == "s.engine":
            base = ""
        elif parent in groups:
            base = groups[parent]
        else:
            sys.exit(
                f"unknown parent group {parent!r} for {var!r} — "
                "the route registration style changed; update this parser"
            )
        groups[var] = (base + "/" + prefix).replace("//", "/") if prefix else base

    routes = set()
    for m in re.finditer(
        rf"\b(\w+)\.({'|'.join(HTTP)})\(\s*\"([^\"]*)\"", src, re.IGNORECASE
    ):
        var, method, path = m.groups()
        if var in ("engine", "s"):
            routes.add((("/" + path).replace("//", "/"), method.lower()))
        else:
            routes.add(
                ((groups[var] + "/" + path).replace("//", "/").rstrip("/") or "/",
                 method.lower())
            )
    return routes


def normalize(route):
    path, method = route
    if path == "/api/login":
        # alias: the auth route lives outside the v1 group, and the swagger
        # op carries a servers override (/api) instead of the /api/v1 base.
        # If login ever moves into the v1 group, remove this alias so the
        # op's server override gets revisited.
        path = "/login"
    elif path.startswith(BASE):
        path = path[len(BASE):]
    path = re.sub(r":(\w+)", r"{\1}", path)
    return (path or "/", method)


def documented_ops():
    spec = yaml.safe_load(open(SWAGGER, encoding="utf-8"))
    ops = set()
    for path, item in (spec.get("paths") or {}).items():
        for method in item:
            if method in HTTP:
                ops.add((path, method))
    return ops


def main():
    code = {normalize(r) for r in registered_routes()}
    code = {(p, m) for p, m in code if not p.endswith("/swagger.yaml")}
    doc = documented_ops()

    missing = sorted(code - doc)
    stale = sorted(doc - code)
    for p, m in missing:
        print(f"in code but NOT in swagger: {m.upper():7s} {p}")
    for p, m in stale:
        print(f"in swagger but NOT in code: {m.upper():7s} {p}")

    if missing or stale:
        print(
            f"\nswagger out of sync: {len(missing)} missing, {len(stale)} stale — "
            "update docs/api/swagger.yaml in the same PR as the route change",
            file=sys.stderr,
        )
        sys.exit(1)
    print(f"swagger in sync: {len(code)} routes == {len(doc)} documented ops")


if __name__ == "__main__":
    main()
