"""Load an OpenAPI 3.0 swagger.yaml into a flat operation catalog.

Each operation: {method, path, tag, summary, path_params, query_params,
documented_responses, response_content, body (json schema descriptor),
is_multipart, security}.
"""
import yaml


def load_spec(path):
    with open(path, "r", encoding="utf-8") as f:
        return yaml.safe_load(f)


def _schema_by_ref(spec, ref):
    if not ref or not ref.startswith("#/"):
        return None
    node = spec
    for part in ref[2:].split("/"):
        if not isinstance(node, dict) or part not in node:
            return None
        node = node[part]
    return node


def resolve_schema(spec, schema, depth=0):
    """Resolve $ref chains into a plain schema dict (one level of allOf merge)."""
    if not isinstance(schema, dict) or depth > 5:
        return schema or {}
    if "$ref" in schema:
        return resolve_schema(spec, _schema_by_ref(spec, schema["$ref"]), depth + 1)
    if "allOf" in schema:
        merged = {}
        for sub in schema["allOf"]:
            sub = resolve_schema(spec, sub, depth + 1)
            for k, v in sub.items():
                if k == "required":
                    merged.setdefault("required", []).extend(v)
                else:
                    merged[k] = v
        return merged
    return schema


def load_operations(path):
    spec = load_spec(path)
    ops = []
    for path_tmpl, path_item in (spec.get("paths") or {}).items():
        shared_params = path_item.get("parameters") or []
        for method, op in path_item.items():
            if method not in ("get", "post", "put", "delete", "patch"):
                continue
            params = list(shared_params) + list(op.get("parameters") or [])
            path_params = [p["name"] for p in params if p.get("in") == "path"]
            query_params = [
                {"name": p["name"],
                 "required": bool(p.get("required")),
                 "schema": p.get("schema") or {}}
                for p in params if p.get("in") == "query"
            ]

            body = None
            is_multipart = False
            req_body = op.get("requestBody")
            if req_body:
                content = (req_body or {}).get("content") or {}
                json_schema = (content.get("application/json") or {}).get("schema")
                if json_schema:
                    resolved = resolve_schema(spec, json_schema)
                    body = {
                        "required": resolved.get("required") or [],
                        "properties": resolved.get("properties") or {},
                        "raw": resolved,
                    }
                elif any(k.startswith("multipart") or k == "application/octet-stream"
                         for k in content):
                    body = {"required": [], "properties": {}, "raw": {}}
                    is_multipart = True

            responses = sorted((op.get("responses") or {}).keys())
            # Declared content types across 2xx responses — lets the validator
            # treat non-JSON bodies (font/ttf, octet-stream) as legitimate
            # instead of path-regex guessing.
            response_content = sorted({
                ct
                for code, resp in (op.get("responses") or {}).items()
                if str(code).startswith("2")
                for ct in ((resp or {}).get("content") or {})
            })
            ops.append({
                "method": method.upper(),
                "path": path_tmpl,
                "tag": (op.get("tags") or ["untagged"])[0],
                "summary": op.get("summary") or "",
                "operation_id": op.get("operationId") or "",
                "path_params": path_params,
                "query_params": query_params,
                "documented_responses": responses,
                "response_content": response_content,
                "body": body,
                "is_multipart": is_multipart,
                "no_auth": op.get("security") == [],
                # op-level servers override the global base for this op
                # (POST /login is served at /api/login, not /api/v1/login).
                "servers": op.get("servers") or [],
            })
    return {"spec": spec, "operations": ops, "info": spec.get("info") or {},
            "servers": spec.get("servers") or []}
