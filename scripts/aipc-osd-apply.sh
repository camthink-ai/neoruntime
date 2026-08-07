#!/bin/sh
# aipc-osd-apply — rewrite the active Hailo medialib OSD config so the encoded
# stream carries only the NE503 text logo (top-left), stripping all vendor demo
# overlays (text "HailoAI"/"Demo Application", datetime stamp, Hailo logo image).
#
# Installed as /usr/libexec/aipc-osd-apply and wired into
# camera-daemon.service::ExecStartPre, so it re-runs before every medialib
# pipeline init: idempotent self-heal across NE503 redeploy, BSP reflash (which
# restores the vendor demo OSD), and camera-daemon restart.
#
# Best-effort by design: OSD is cosmetic, so any failure logs a warning and
# exits 0 — it must NEVER block camera-daemon from starting.
#
# Logo text: /data/aipc/etc/osd-logo.conf (one line, default "Camthink").
# Change it at runtime, then: systemctl restart camera-daemon.

LOGO_CONF="/data/aipc/etc/osd-logo.conf"
DEFAULT_LOGO=""
FONT_PATH="/usr/share/fonts/ttf/LiberationMono-Regular.ttf"

if ! command -v python3 >/dev/null 2>&1; then
    echo "aipc-osd-apply: python3 not found; skipping (exit 0)" >&2
    exit 0
fi

# The python block always exits 0; see note above.
# argv: logo_conf default_logo font_path
python3 - "$LOGO_CONF" "$DEFAULT_LOGO" "$FONT_PATH" <<'PYEOF'
import glob, json, os, sys

logo_conf, default_logo, font_path = sys.argv[1:4]


def load_logo():
    try:
        with open(logo_conf) as f:
            t = f.read().strip()
        return t or default_logo
    except OSError:
        return default_logo


def build_logo_entry():
    # Mirrors the proven vendor "Demo Application" top-left entry (x/y/z-index,
    # font, color, rotation_policy) so it renders reliably; only the label is
    # swapped for the NE503 brand text.
    e = {
        "angle": 0,
        "font_size": 80,
        "id": "ne503_logo",
        "label": LOGO,
        "rotation_policy": "CENTER",
        "text_color": [255, 255, 255],
        "x": 0.0,
        "y": 0.01,
        "z-index": 1,
    }
    if os.path.isfile(font_path):
        e["font_path"] = font_path
    return e


def atomic_write(path, obj):
    tmp = path + ".ne503-tmp"
    with open(tmp, "w") as f:
        json.dump(obj, f, indent=2)
        f.write("\n")
    os.replace(tmp, path)


try:
    LOGO = load_logo()

    # One active medialib config per device; glob tolerates SoC (hailo15h/l) and
    # sensor (imx*) variants. 93.72 is hailo15h/imx664/theia_sl410m/4mp.
    tops = glob.glob(
        "/etc/imaging/cfg/hailo15[hl]/imx*/theia_sl410m/*/"
        "medialib_configs/webserver_medialib_config.json"
    )
    if not tops:
        print("aipc-osd-apply: no medialib config found; nothing to do",
              file=sys.stderr)
        sys.exit(0)

    patched = 0
    for top in tops:
        try:
            with open(top) as f:
                tc = json.load(f)
        except (OSError, ValueError):
            continue
        default = tc.get("default_profile")
        if not default:
            continue
        prof = next((p for p in tc.get("profiles", [])
                     if p.get("name") == default), None)
        if not prof:
            continue
        pcfg = prof.get("config_file")
        if not pcfg or not os.path.isfile(pcfg):
            continue
        try:
            with open(pcfg) as f:
                pd = json.load(f)
        except (OSError, ValueError):
            continue
        base = os.path.dirname(pcfg)
        for stream in pd.get("encoded_output_streams", []):
            osd_rel = stream.get("osd")
            if not osd_rel:
                continue
            osd_path = (osd_rel if os.path.isabs(osd_rel)
                        else os.path.join(base, osd_rel))
            if not os.path.isfile(osd_path):
                continue
            try:
                with open(osd_path) as f:
                    od = json.load(f)
            except (OSError, ValueError):
                continue
            # One-time pristine backup for rollback. Only the `osd` key is
            # rewritten; metadata.content_hash is left untouched so the Hailo
            # webserver does not flag the file as externally corrupted.
            bak = osd_path + ".ne503-orig"
            if not os.path.exists(bak):
                try:
                    with open(osd_path, "rb") as src, open(bak, "wb") as dst:
                        dst.write(src.read())
                except OSError:
                    pass
            od["osd"] = {
                "dateTime": [],
                "image": [],
                "text": [build_logo_entry()],
            }
            atomic_write(osd_path, od)
            patched += 1

    print("aipc-osd-apply: logo=%r patched %d osd file(s)"
          % (LOGO, patched), file=sys.stderr)
except Exception as e:  # cosmetic helper: never block camera-daemon
    print("aipc-osd-apply: warning: %s (exit 0)" % e, file=sys.stderr)
PYEOF
