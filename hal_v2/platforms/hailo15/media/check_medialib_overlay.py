#!/usr/bin/env python3
"""
Build-time consistency check between the two repo-managed imaging trees.

The compiled-in default medialib config (default_medialib_overlay, embedded in
the HAL) and the pack-release imaging overlay (configs/imaging, installed to
$PREFIX/etc/imaging) are maintained in parallel: the overlay is a strict subset
of the pack tree. Nothing in the build forces the two to stay in sync, so a
retuned profile committed to one tree but not the other silently diverges —
bench units (compiled-in default) and packed units (configs/imaging) then
behave differently and the miss only surfaces in the field.

This check runs before bundle generation and fails the build on:

  1. Container drift — every profile entry (name -> config_file) in the
     overlay's webserver_medialib_config.json must exist with the identical
     config_file in the pack tree's container. The pack container may carry
     extra profiles (Detection/FaceLandmarks variants); the overlay may not.
  2. File drift — every file under the overlay's etc/imaging/cfg/hailo15h
     profile tree must exist byte-identical at the same relative path under
     the pack tree's cfg/hailo15h. This is what catches a retuned
     iq_settings.json committed to one tree only.
  3. Dangling refs — every container entry's config_file must resolve inside
     the pack tree or the SDK sysroot (the pack layout is SDK base + repo
     overlay, mirroring how the bundle generator resolves overlay + sysroot),
     so a renamed/removed profile dir fails here rather than at camera boot
     on the device.

Usage:
  check_medialib_overlay.py --overlay <default_medialib_overlay> \
                            --repo-configs <configs/imaging>
"""
import argparse
import filecmp
import json
import os
import sys

OVERLAY_CONTAINER_REL = "etc/imaging/cfg/medialib_configs/webserver_medialib_config.json"
REPO_CONTAINER_REL = os.path.join("hailo15h", "imx678", "theia_sl410m", "4k",
                                  "medialib_configs", "webserver_medialib_config.json")
PROFILE_TREE_REL = os.path.join("cfg", "hailo15h")


def _fail(msg):
    sys.exit(f"check_medialib_overlay: FAIL: {msg}")


def _ref_exists(cf, roots):
    """True if container ref `cf` (an /etc/imaging/... absolute path) resolves
    under any root. A root either contains the full etc/imaging prefix (SDK
    sysroot) or is the imaging tree itself (repo overlay maps /etc/imaging/X
    to X), so both forms are tried."""
    rel = cf.lstrip("/")
    stripped = cf[len("/etc/imaging/"):] if cf.startswith("/etc/imaging/") else None
    for root in roots:
        for cand in (os.path.join(root, rel), os.path.join(root, stripped) if stripped else None):
            if cand and os.path.isfile(cand):
                return True
    return False


def _load(path):
    try:
        with open(path, "r") as f:
            return json.load(f)
    except Exception as e:
        _fail(f"cannot parse {path}: {e}")


def _entries(container, path):
    profs = container.get("profiles")
    if not isinstance(profs, list):
        _fail(f"{path} has no profiles list")
    out = {}
    for p in profs:
        name, cf = p.get("name"), p.get("config_file")
        if not isinstance(name, str) or not isinstance(cf, str):
            _fail(f"{path} has a malformed profile entry: {p!r}")
        out[name] = cf
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--overlay", required=True,
                    help="default_medialib_overlay root (compiled-in tree)")
    ap.add_argument("--repo-configs", required=True,
                    help="configs/imaging root (pack-release tree)")
    ap.add_argument("--sysroot", default="",
                    help="Optional SDK sysroot; the pack tree is SDK base + repo "
                         "overlay, so container refs may resolve in either.")
    ap.add_argument("--profiles", default="",
                    help="Comma-separated profile list embedded in the compiled-in "
                         "bundle (HAL_V2_DEFAULT_MEDIALIB_PROFILES). Every overlay "
                         "container entry must appear in it: the bundle generator "
                         "silently drops container entries not on the list, so a "
                         "forgotten CMake update would ship bench/field divergence.")
    args = ap.parse_args()

    overlay = args.overlay.rstrip("/")
    repo = args.repo_configs.rstrip("/")
    sysroot = args.sysroot.strip().rstrip("/") if args.sysroot.strip() else ""

    # Search roots for container config_file refs, in pack-overlay-first order.
    repo_roots = [repo] + ([sysroot] if sysroot else [])

    overlay_container_path = os.path.join(overlay, OVERLAY_CONTAINER_REL)
    repo_container_path = os.path.join(repo, REPO_CONTAINER_REL)
    for p in (overlay_container_path, repo_container_path):
        if not os.path.isfile(p):
            _fail(f"container missing: {p}")

    # 1. Container subset: overlay entries must match the pack tree exactly.
    ov_entries = _entries(_load(overlay_container_path), overlay_container_path)
    rp_entries = _entries(_load(repo_container_path), repo_container_path)
    for name, cf in sorted(ov_entries.items()):
        if name not in rp_entries:
            _fail(f"profile '{name}' is in the overlay container but not in "
                  f"{repo_container_path} — add it to both trees")
        if rp_entries[name] != cf:
            _fail(f"profile '{name}' config_file differs: overlay={cf} "
                  f"repo={rp_entries[name]}")

    # 1b. Embed-list gate: the bundle trims to --profiles, so an overlay
    # container entry missing from the CMake list never reaches the bench.
    embed = [s.strip() for s in args.profiles.split(",") if s.strip()]
    if embed:
        dropped = [n for n in sorted(ov_entries) if n not in embed]
        if dropped:
            _fail(f"profile(s) {dropped} are in the overlay container but not in "
                  f"the embed list (HAL_V2_DEFAULT_MEDIALIB_PROFILES); the bundle "
                  f"generator would silently drop them — update the CMake list")

    # 2/3. File-level subset over the shared cfg/hailo15h profile tree, plus
    # existence of every container entry's config_file inside its own tree.
    ov_root = os.path.join(overlay, "etc", "imaging", PROFILE_TREE_REL)
    if not os.path.isdir(ov_root):
        # os.walk over a missing dir yields nothing — without this guard a
        # deleted overlay profile tree would pass vacuously with 0 files.
        _fail(f"overlay profile tree missing: {ov_root}")
    rp_root = os.path.join(repo, PROFILE_TREE_REL)
    checked = 0
    for dirpath, _dirnames, filenames in os.walk(ov_root):
        rel_dir = os.path.relpath(dirpath, ov_root)
        for fn in filenames:
            rel = os.path.join(rel_dir, fn) if rel_dir != "." else fn
            rp_file = os.path.join(rp_root, rel)
            if not os.path.isfile(rp_file):
                _fail(f"overlay file {rel} has no counterpart under {rp_root}")
            if not filecmp.cmp(os.path.join(dirpath, fn), rp_file, shallow=False):
                _fail(f"overlay file {rel} differs from {rp_file} — commit the "
                      f"retuned file to BOTH trees")
            checked += 1

    for name, cf in sorted(rp_entries.items()):
        if not _ref_exists(cf, repo_roots):
            _fail(f"profile '{name}' config_file not readable in {repo_container_path} "
                  f"(or sysroot): {cf}")

    print(f"check_medialib_overlay: OK ({len(ov_entries)}/{len(rp_entries)} entries "
          f"consistent, {checked} profile files identical)", file=sys.stderr)


if __name__ == "__main__":
    main()
