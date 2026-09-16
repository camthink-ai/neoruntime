"""Instance-ID discovery for templated paths.

Maps path template -> (list endpoint, container field, id field candidates).
Discovery is defensive: the response envelope may nest the list under
data / data.<container> / data itself; first matching id field wins.
"""

# path-template prefix -> discovery recipe (list endpoint, container field,
# param name produced by that list, id field candidates). Keys are matched
# against templated op paths ("/store/apps/{key}" matches "/store/apps"),
# so same-named params under different parents never collide.
RECIPES = {
    "/ai/models": ("/ai/models", ["models"], "model_id", ["model_id", "id", "name"]),
    "/apps": ("/apps", ["apps"], "app_id", ["app_id", "id"]),
    "/containers": ("/containers", [], "id", ["id"]),
    "/streams": ("/streams", ["streams"], "stream_id", ["stream_id", "id"]),
    "/settings": ("/settings", ["settings"], "key", None),  # dict keys
    "/processes": ("/processes", [], "pid", ["pid", "id"]),
    "/images": ("/images", [], "image", ["name", "id"]),
    "/store/installs": ("/store/installs", [], "app_id", ["app_id", "id"]),
    "/store/apps": ("/store/apps", ["apps"], "key", ["key", "app_key", "id", "name"]),
    "/dev/projects": ("/dev/projects", [], "id", ["id"]),
}


def params_for(op_path, instances):
    """Longest-prefix match: {param: value} for one op path, or {}."""
    best = None
    for prefix in instances:
        if op_path.startswith(prefix) and (best is None or len(prefix) > len(best)):
            best = prefix
    return dict(instances[best]) if best else {}


def _walk_container(payload, container_keys):
    """Return the first list found under data[data.<key>]; None if absent."""
    data = payload.get("data") if isinstance(payload, dict) else payload
    if isinstance(data, list):
        return data
    if not isinstance(data, dict):
        return None
    for key in container_keys:
        node = data.get(key)
        if isinstance(node, list):
            return node
        if isinstance(node, dict) and container_keys == ["settings"]:
            return node  # settings: dict of key -> value
    return None


def _first_id(item, id_fields):
    if not isinstance(item, dict):
        return None
    for field in id_fields:
        value = item.get(field)
        if isinstance(value, (str, int)) and str(value) != "":
            return str(value)
    return None


def discover_all(get_json, log):
    """get_json(path) -> parsed JSON or None.

    Returns {template_prefix: {param: id|None}}.
    """
    instances = {}
    for prefix, (list_path, container_keys, param, id_fields) in RECIPES.items():
        payload = get_json(list_path)
        found = None
        if payload is None:
            log(f"discover {list_path}: request failed")
        else:
            node = _walk_container(payload, container_keys)
            if container_keys == ["settings"]:
                # dict keys: pick the first key. W-tier DELETE uses its own
                # self-created key, so this is only a fallback instance.
                found = next(iter(node.keys()), None) if isinstance(node, dict) else None
            elif isinstance(node, list) and node:
                for item in node:
                    found = _first_id(item, id_fields or ["id"])
                    if found is not None:
                        break
        instances[prefix] = {param: found}
        log(f"discover {list_path} -> {prefix}.{{{param}}}: "
            f"{'=' + str(found) if found else 'no instance'}")
    return instances


def writeback_snapshot(get_json):
    """Capture current values for read-modify-writeback W-tier ops."""
    snapshots = {}
    for name, path in (("device_info", "/device-info"),
                       ("media_config", "/media/config")):
        payload = get_json(path)
        data = payload.get("data") if isinstance(payload, dict) else None
        if data is not None:
            snapshots[name] = data
    return snapshots
