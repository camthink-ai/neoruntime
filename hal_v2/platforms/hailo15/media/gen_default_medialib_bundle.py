#!/usr/bin/env python3
"""
Generate a self-contained, compiled-in default media-library config bundle for the
Hailo-15 HAL.

The Hailo media library requires every profile to carry a readable on-disk `config_file`
(parsed + flattened at init), so a config cannot be purely in-memory. To make the HAL
self-contained (no dependency on /etc/imaging/cfg/...), this script:

  1. Reads the SDK's webserver_medialib_config.json (the medialib container).
  2. Keeps only the requested profiles (default: the five *_Basic profiles).
  3. Recursively gathers every .json config file reachable from each profile's config_file
     (section refs, encoder/osd/masking refs, ...). Data files (.txt/.hef/missing) are left
     untouched — they are runtime assets that live on the device.
  4. Rewrites every reference whose target is in the gathered set to point under a fixed
     scratch root, so at runtime the HAL only has to write the files there.
  5. Strips OSD overlays (dateTime/image/text) and static privacy-mask polygons from every
     embedded file, so the compiled-in default starts clean (no demo OSD, no pre-set masks).
     Dynamic privacy-mask config and mask_type/color settings are preserved.
  6. Emits a single C++ source file defining `kHailo15DefaultMedialibBundle` — a JSON blob
     with the rewritten container + the {scratch_path: content} map.

Usage:
  gen_default_medialib_bundle.py \
      --sysroot <SDK sysroot> \
      --webserver-config <rel path under sysroot, default etc/imaging/cfg/medialib_configs/webserver_medialib_config.json> \
      --scratch-root <runtime scratch dir, default /var/tmp/hal_medialib_default> \
      --profiles Daylight_Basic,High_Dynamic_Range_Basic,AI_ISP_Gen1_Basic,AI_ISP_Gen2_Basic,AI_ISP_Gen3_Basic \
      --out <generated .cpp path>
"""
import argparse
import copy
import hashlib
import json
import os
import sys


# ---- Normalized 3-stream layout applied to every embedded profile ----
# (stream_id, width, height, target_bitrate). Framerate is DEFAULT_FRAMERATE,
# pool_max_buffers is DEFAULT_POOL_MAX_BUFFERS for every stream. target_bitrate scales
# roughly with pixel count so each stream gets a sensible H264 default.
DEFAULT_STREAMS = [
    ("sink0", 1920, 1080, 6_000_000),  # 1080P ~6 Mbps
    ("sink1", 1280, 720,  3_000_000),  # 720P   ~3 Mbps
    ("sink2", 640, 384,   1_000_000),  # low    ~1 Mbps
]
DEFAULT_FRAMERATE = 30
DEFAULT_POOL_MAX_BUFFERS = 20


# ---- content_hash: faithful port of medialib ConfigValidator::calculate_json_hash ----
# sha256 over the canonical incremental serialization (sorted object keys; floats as
# fixed %.8f; other scalars via nlohmann dump), with the "metadata" field excluded.
# The medialib validates this (warns on mismatch), so it must be recomputed after any
# content change (normalization + scratch-path rewriting).

def _canonical_scalar(v):
    if isinstance(v, bool):
        return "true" if v else "false"
    if isinstance(v, float):
        return f"{v:.8f}"  # std::fixed << std::setprecision(8)
    if isinstance(v, int):
        return str(v)
    if v is None:
        return "null"
    if isinstance(v, str):
        return json.dumps(v, ensure_ascii=True)  # nlohmann json.dump(-1, ' ', true)
    raise TypeError(f"unhashable scalar type: {type(v)}")


def _hash_feed(j, h):
    if isinstance(j, dict):
        h.update(b"{")
        keys = sorted(j.keys())
        for i, k in enumerate(keys):
            h.update(json.dumps(k, ensure_ascii=True).encode())  # nlohmann::json(key).dump()
            h.update(b":")
            _hash_feed(j[k], h)
            if i < len(keys) - 1:
                h.update(b",")
        h.update(b"}")
    elif isinstance(j, list):
        h.update(b"[")
        for i, v in enumerate(j):
            _hash_feed(v, h)
            if i < len(j) - 1:
                h.update(b",")
        h.update(b"]")
    else:
        h.update(_canonical_scalar(j).encode())


def content_hash(obj):
    """sha256 hex digest of obj (with its 'metadata' field removed), matching the medialib."""
    j = copy.deepcopy(obj)
    if isinstance(j, dict):
        j.pop("metadata", None)
    h = hashlib.sha256()
    _hash_feed(j, h)
    return h.hexdigest()


def recompute_content_hash(obj):
    """Recompute metadata.content_hash in-place if the object has a metadata block."""
    if isinstance(obj, dict) and isinstance(obj.get("metadata"), dict) and "content_hash" in obj["metadata"]:
        obj["metadata"]["content_hash"] = content_hash(obj)


def _load_json(abs_path):
    try:
        with open(abs_path, "r") as f:
            return json.load(f)
    except Exception:
        return None


def _follow_ref(v, files, sysroot):
    """If v is a .json path that exists in the sysroot, load + register + recurse into it."""
    if isinstance(v, str) and v.endswith(".json"):
        abs_path = os.path.join(sysroot, v.lstrip("/"))
        if v not in files and os.path.isfile(abs_path):
            content = _load_json(abs_path)
            if isinstance(content, (dict, list)):
                files[v] = content
                _walk_and_collect(content, files, sysroot)


def _walk_and_collect(obj, files, sysroot):
    """Recursively follow .json string refs that exist in the sysroot, registering them in
    `files` (keyed by original path). Shared files are naturally deduped."""
    if isinstance(obj, dict):
        for v in obj.values():
            if isinstance(v, str):
                _follow_ref(v, files, sysroot)
            elif isinstance(v, (dict, list)):
                _walk_and_collect(v, files, sysroot)
    elif isinstance(obj, list):
        for it in obj:
            if isinstance(it, str):
                _follow_ref(it, files, sysroot)
            elif isinstance(it, (dict, list)):
                _walk_and_collect(it, files, sysroot)


def gather_files(sysroot, container, wanted_names):
    """Return {orig_abs_path: parsed_json} for every .json config file reachable from the
    wanted profiles."""
    files = {}
    for p in container.get("profiles", []):
        if p.get("name") in wanted_names:
            cf = p.get("config_file")
            if isinstance(cf, str) and cf not in files:
                abs_path = os.path.join(sysroot, cf.lstrip("/"))
                content = _load_json(abs_path)
                if content is None:
                    sys.exit(f"gen_default_medialib_bundle: profile '{p['name']}' config_file not readable: {cf}")
                files[cf] = content
                _walk_and_collect(content, files, sysroot)
    return files


def _sibling_path(ref, orig_id, new_id):
    """Path of a per-stream file for new_id, derived from the sink0 file by replacing the
    stream id token in the basename (e.g. encoder_sink0.json -> encoder_sink1.json)."""
    d = os.path.dirname(ref)
    base = os.path.basename(ref).replace(orig_id, new_id)
    return os.path.join(d, base)


def _clone_stream_file(files, ref, orig_id, new_id):
    """Deep-copy an already-gathered per-stream file (encoder/masking/osd) under a sibling
    path for new_id. Registers the clone in `files` and returns its path (unmodified)."""
    new_path = _sibling_path(ref, orig_id, new_id)
    files[new_path] = copy.deepcopy(files[ref])
    return new_path


def _configure_encoder(files, enc_ref, w, h, bitrate):
    """Set input_stream width/height/framerate and hailo_encoder rate_control bitrate on an
    encoder file already in `files`. Bitrate is only applied to hailo_encoder configs (H264/
    HEVC); MJPEG (jpeg_encoder) is left untouched."""
    if not isinstance(enc_ref, str) or enc_ref not in files:
        return
    enc = files[enc_ref]
    try:
        istream = enc["encoding"]["input_stream"]
        istream["width"] = w
        istream["height"] = h
        istream["framerate"] = DEFAULT_FRAMERATE
    except (KeyError, TypeError):
        sys.exit(f"gen_default_medialib_bundle: encoder file '{enc_ref}' missing encoding.input_stream")
    he = enc.get("encoding", {}).get("hailo_encoder")
    if isinstance(he, dict):
        he.setdefault("rate_control", {}).setdefault("bitrate", {})["target_bitrate"] = bitrate


def _strip_osd_and_static_mask(obj):
    """Clear OSD overlays and static privacy-mask polygons from an embedded config dict so the
    compiled-in default starts clean (no demo OSD text/images/datetimes, no pre-set static masks).

    Operates at the JSON level (before the medialib parses it into structs):
      - osd.{dateTime,image,text} -> emptied (arrays kept empty for schema validity).
      - masking.static_privacy_mask / privacy_mask.static_privacy_mask -> enabled=False, masks=[].
    Dynamic privacy-mask config, mask_type, color_value, pixelization_size etc. are preserved.
    Handles standalone osd/masking files as well as encoder files with inline osd/privacy_mask.
    """
    if not isinstance(obj, dict):
        return
    osd = obj.get("osd")
    if isinstance(osd, dict):
        for k in ("dateTime", "image", "text"):
            if isinstance(osd.get(k), list):
                osd[k] = []
    # Standalone masking file uses top-level "masking"; some encoder files inline it as
    # "privacy_mask". Clear the static mask in either form (keep dynamic mask + mask_type).
    for mk in ("masking", "privacy_mask"):
        m = obj.get(mk)
        if isinstance(m, dict):
            spm = m.get("static_privacy_mask")
            if isinstance(spm, dict):
                spm["enabled"] = False
                spm["masks"] = []


def normalize_profile(files, profile_path):
    """Rewrite one profile to the fixed 3-stream layout (1080P / 720P / 640x384, pool 8, 30fps,
    per-resolution bitrate).

    - application_input_streams.resolutions -> 3 entries.
    - encoded_output_streams -> 3 entries; sink1/sink2 get cloned encoder/masking/osd files
      (derived from sink0). Each encoder gets its dimensions + bitrate set per resolution.
    """
    if profile_path not in files:
        return
    profile = files[profile_path]

    # application_input_streams.resolutions
    app_ref = profile.get("application_settings")
    if not isinstance(app_ref, str) or app_ref not in files:
        sys.exit(f"gen_default_medialib_bundle: profile '{profile_path}' missing application_settings ref")
    app = files[app_ref]
    app.setdefault("application_input_streams", {})["resolutions"] = [
        {
            "stream_id": sid,
            "width": w,
            "height": h,
            "framerate": DEFAULT_FRAMERATE,
            "pool_max_buffers": DEFAULT_POOL_MAX_BUFFERS,
        }
        for (sid, w, h, _br) in DEFAULT_STREAMS
    ]

    # Force hailort service mode on by default (the HAL talks to the device in-process via
    # the hailort service; matches the service-mode behavior enforced by media_library_service).
    hailort = app.setdefault("hailort", {})
    if isinstance(hailort, dict):
        hailort.setdefault("device-id", "device0")
        hailort["use-hailort-service"] = True

    # Force lens dewarp (anti-distortion) off by default.
    iq_ref = profile.get("iq_settings")
    if isinstance(iq_ref, str) and iq_ref in files:
        files[iq_ref].setdefault("dewarp", {})["enabled"] = False

    # encoded_output_streams
    eos = profile.get("encoded_output_streams")
    if not isinstance(eos, list) or not eos:
        sys.exit(f"gen_default_medialib_bundle: profile '{profile_path}' has no encoded_output_streams")
    base = eos[0]
    orig_id = base.get("stream_id", "sink0")
    enc0 = base.get("encoding")
    mask0 = base.get("masking")
    osd0 = base.get("osd")

    new_eos = []
    for idx, (sid, w, h, br) in enumerate(DEFAULT_STREAMS):
        if idx == 0:
            # sink0: reuse the original encoder file, force its dims + bitrate to the 1080P
            # defaults (the source profile is 4K @ 16Mbps). masking/osd are resolution-agnostic.
            _configure_encoder(files, enc0, w, h, br)
            entry = {"stream_id": sid, "encoding": enc0, "masking": mask0, "osd": osd0}
        else:
            enc_new = _clone_stream_file(files, enc0, orig_id, sid)
            _configure_encoder(files, enc_new, w, h, br)
            entry = {
                "stream_id": sid,
                "encoding": enc_new,
                "masking": _clone_stream_file(files, mask0, orig_id, sid) if isinstance(mask0, str) and mask0 in files else mask0,
                "osd": _clone_stream_file(files, osd0, orig_id, sid) if isinstance(osd0, str) and osd0 in files else osd0,
            }
        new_eos.append(entry)
    profile["encoded_output_streams"] = new_eos


def rewrite_refs(obj, orig_to_scratch):
    """In-place replace any string value that exactly equals a known orig path with its
    scratch path. Returns the same object."""
    if isinstance(obj, dict):
        for k in list(obj.keys()):
            v = obj[k]
            if isinstance(v, str):
                if v in orig_to_scratch:
                    obj[k] = orig_to_scratch[v]
            else:
                rewrite_refs(v, orig_to_scratch)
    elif isinstance(obj, list):
        for i, it in enumerate(obj):
            if isinstance(it, str):
                if it in orig_to_scratch:
                    obj[i] = orig_to_scratch[it]
            else:
                rewrite_refs(it, orig_to_scratch)
    return obj


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sysroot", required=True)
    ap.add_argument("--webserver-config",
                    default="etc/imaging/cfg/medialib_configs/webserver_medialib_config.json")
    ap.add_argument("--scratch-root", default="/var/tmp/hal_medialib_default")
    ap.add_argument("--profiles",
                    default="Daylight_Basic,High_Dynamic_Range_Basic,AI_ISP_Gen1_Basic,AI_ISP_Gen2_Basic,AI_ISP_Gen3_Basic")
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    sysroot = args.sysroot.rstrip("/")
    wanted = set(p.strip() for p in args.profiles.split(",") if p.strip())

    wc_path = os.path.join(sysroot, args.webserver_config.lstrip("/"))
    if not os.path.isfile(wc_path):
        sys.exit(f"gen_default_medialib_bundle: webserver config not found: {wc_path}")
    with open(wc_path, "r") as f:
        container = json.load(f)

    # Trim container to wanted profiles; preserve other top-level fields.
    kept = [p for p in container.get("profiles", []) if p.get("name") in wanted]
    missing = wanted - set(p.get("name") for p in kept)
    if missing:
        sys.exit(f"gen_default_medialib_bundle: profiles not found in container: {sorted(missing)}")
    container["profiles"] = kept
    # Point default_profile at one of the kept profiles if the original default was dropped.
    if container.get("default_profile") not in wanted:
        container["default_profile"] = sorted(wanted)[0]

    files = gather_files(sysroot, container, wanted)

    # Normalize every profile to the fixed 3-stream layout (1080P / 720P / 640x384, pool 8,
    # 30fps). This mutates each profile + its application_settings and registers cloned
    # per-stream files (encoder/masking/osd for sink1/sink2) in `files`.
    for p in container["profiles"]:
        normalize_profile(files, p["config_file"])

    # Re-walk newly added cloned files in case they reference further .json files.
    for content in list(files.values()):
        _walk_and_collect(content, files, sysroot)

    # Strip demo OSD overlays + static privacy masks so the compiled-in default starts clean
    # (the SDK's Basic profiles ship with example OSD text/images and may carry static masks).
    # Done after cloning so all per-stream osd/masking files (sink0 original + sink1/sink2
    # clones) are covered, and before content_hash recomputation so hashes stay consistent.
    for content in files.values():
        _strip_osd_and_static_mask(content)

    # Map each embedded orig path to a scratch path (scratch_root + full orig path).
    orig_to_scratch = {orig: args.scratch_root.rstrip("/") + orig for orig in files}

    # Rewrite refs inside every embedded file and inside the container.
    for content in files.values():
        rewrite_refs(content, orig_to_scratch)
    rewrite_refs(container, orig_to_scratch)

    # Recompute metadata.content_hash for every embedded file + the container. The content
    # changed (3-stream normalization + scratch-path rewriting), so the original hashes no
    # longer match; the medialib validates this field (warns on mismatch).
    for content in files.values():
        recompute_content_hash(content)
    recompute_content_hash(container)

    # Build the bundle: scratch_root + rewritten container + {scratch_path: content}.
    files_out = {orig_to_scratch[orig]: content for orig, content in files.items()}
    bundle = {
        "scratch_root": args.scratch_root.rstrip("/"),
        "container": container,
        "files": files_out,
    }

    bundle_str = json.dumps(bundle, separators=(",", ":"))

    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as f:
        f.write("/* AUTO-GENERATED by gen_default_medialib_bundle.py — do not edit. */\n")
        f.write("/* Self-contained default media-library config (5 Basic profiles), baked into the HAL. */\n")
        f.write('extern const char *kHailo15DefaultMedialibBundle =\n')
        f.write('    R"BUNDLE(' + bundle_str + ')BUNDLE";\n')

    n = len(files)
    sz = len(bundle_str)
    print(f"gen_default_medialib_bundle: embedded {n} files ({sz} bytes) for profiles {sorted(wanted)} "
          f"-> {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
