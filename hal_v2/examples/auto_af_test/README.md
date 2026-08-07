# auto_af_test (HAL V2)

`hal-auto-af-test` is a CLI demo that implements autofocus using:

- **ISP AF statistics**: `HAL_ISP_OPS.set_af_windows_config()` + `HAL_ISP_OPS.get_af_measurement()`
- **Lens control**: `HAL_MCU_OPS` + `HAL_LENS_OPS` via the AF0832 helper (`hal_lens_af0832_*`)
- **AF window + curve overlays**: `HAL_OSD_OPS` **CustomOverlay** (ARGB) on the encoder output
- **Frame sync**: optional `HAL_VIDEO_OPS.subscribe_stream()` on the selected video context so lens moves can align ISP reads to frontend frames

## Build

This example is built as part of the `hal_v2` CMake build when **MediaLibrary is available** (`HAVE_HAILO_MEDIALIB`).

Target:

- `hal-auto-af-test`

## Run

```
hal-auto-af-test <medialib_json_path> [--backup <dir>]
```

On start, it initializes and starts the media pipeline, prints available `video` / `codec` streams, and enters an interactive CLI.

## Quick start (typical flow)

1) Select AF stats source (`vidx`) and OSD target encoder (`cidx`) if needed:

- `streams`
- `vidx 0`
- `cidx 0`

2) Configure AF windows (pixels in the selected video stream coordinates):

- `isp_af_set 0 1 x y w h`

3) Enable AF overlays (AF windows + focus metric curve after each `af_run_once`):

- `af_osd 1`
- (optional) `af_osd_style thickness a r g b`

4) Tune AF (optional; defaults are reasonable to start):

- `af_param_get`
- e.g. `af_param_set sync_frames 1`, `af_param_set metric_avg_frames 5`

5) Initialize MCU + lens:

- `mcu_init /dev/ttyS0 115200 2000`
- `af_create`

6) Run autofocus:

- `af_run_once`
- or enable tracking: `af_track 1`

7) Zoom (uses AF0832 tracking table; pauses AF to avoid conflicts):

- `zoom_goto 1.5 3.0`

8) (Optional) UDP RTP push (H.264/H.265):

- `udp_start 127.0.0.1 5004`
- or select a specific encoder: `udp_select 0 127.0.0.1 5004`
- `udp_stats`
- `udp_stop`

## ISP AF measurements (Hailo-15)

See `HalIspAfMeasurement` in `hal_v2/include/media/hal_isp.h` for the full comment. In short:

- **`sum`**: hardware focus / clarity energy; it tends to **peak** at best focus — the demo **maximizes** a metric derived from `sum` and `luma`.
- **`luma`**: mean brightness in the window; used together with `sum` (e.g. ratio). Large exposure swings can disturb `sum` readings.
- **All zeros**: usually means AF stats are not running (AF not enabled, invalid windows, or overflow per tuning guide), not “extreme sharpness”.

The demo combines per-window **`sum / (luma+1)`** with weights, then **averages** (see below). **`metric_ratio_cap`**: if set to a **positive** value, each window’s ratio is **capped** (this flattens peaks on the plot). **Default is `0` (no cap).**

## Autofocus algorithm (`af_run_once`)

Two phases use the **same** scan helper `af_scan_expand_from_center`:

1. **Coarse**
   - Interval: `[c0, c1]` — either full `[min_pos, max_pos]` when `coarse_span_steps == 0`, or `[current_focus ± coarse_span_steps]` clamped to the legal range.
   - Start at the **current lens position** (or midpoint if unknown).
   - **Expand** right in steps of `coarse_step`, then left in steps of `coarse_step`, sampling `(focus_pos, metric)` at each stop.
   - **Early stop** (optional): after enough distinct samples, a **global** least-squares **quadratic** fit on all samples so far is used; if it is downward-opening and the scan has passed the fitted vertex far enough on that side, extension on that side stops. Otherwise the scan runs until the interval boundary.
   - **Anti false-stop**: before early-stop may fire, sampled focus positions must span at least **`min(early_stop_min_spread, interval_width)`** pulses (`early_stop_min_spread`, default **128**). This avoids stopping after only a few noisy samples, which used to skew `coarse_peak_x` and cause apparent “sharp triangle then flat line” misfocus. Set **`early_stop_min_spread 0`** to restore the old aggressive behaviour (faster but riskier).
   - **`coarse_peak_x`**: fitted vertex if the fit is trusted; else the position of the **discrete** maximum metric.

2. **Fine**
   - **Center**: `round(clamp(coarse_peak_x))` within `[min_pos, max_pos]`, **unless** that differs from the coarse **discrete** best by more than `coarse_step` — then the discrete best is used (LS fit can place the vertex away from the true peak on asymmetric / noisy curves).
   - **Span**: `span = max(fine_step, fine_span_steps)` in focus units; scan `[fine_center − span, fine_center + span]` again clamped to legal range.
   - **Step**: `fine_step` (same expand/early-stop logic as coarse).
   - **Final position**: the **discrete** argmax over fine samples (`best_f` after the fine scan), **not** `round(fine_peak_x)` — fitted `fine_peak_x` is logged only and can land on a scan edge with a poor metric.
   - After the final move, the demo reads the metric once more (with the same settle + average settings) for the printed `best_m`.

**Sampling at each focus step:** lens absolute move → wait **`sync_frames`** frontend frames (video subscribe when available, else FPS-based sleep) → read **`metric_avg_frames`** consecutive ISP samples, **one frame apart**, and **average** them into one scalar for that point.

**Tracking (`af_track 1`):** periodically reads a **single** ISP sample (no multi-frame average) to detect large drops and re-queue a scan.

### Debug logs (stdout)

Each `af_run_once` prints **`af_scan [coarse|fine] …`** lines:

- **start**: center, `[lo,hi]`, step, early-stop tuning, move counter.
- **right / left**: **`skipped`** if there is no room to step toward that boundary; **`early_stop`** when the global quadratic early-stop fired; **`boundary`** when the scan ran out of interval (hit `hi` / `lo`) without early-stop.
- **aborted**: `af_stop`, `af_pause`, **`max_total_moves`**, or measure / lens error (with `hal_error_to_string`).
- **done**: sample count and running best discrete peak from the phase.

The focus curve OSD (when `af_osd 1`) marks the **chosen peak** with a circle + crosshair and a **`P=<pos> m=<metric>`** bitmap label (GLCD 5×7 font, same glyph table as `hal_draw_cpu.cpp` / Adafruit glcdfont).

## CLI parameters (`af_param_set` / `af_param_get`)

| Name | Meaning |
|------|---------|
| `pps` | Lens motion speed (pulses per second) for AF0832 moves |
| `min_pos` / `max_pos` | Focus travel limits |
| `coarse_span_steps` | `0` = full range; else scan ± this many steps around current focus |
| `coarse_step` / `fine_step` | Focus step size for coarse / fine phase |
| `fine_span_steps` | Used with `fine_step` as `span = max(fine_step, fine_span_steps)` for the fine scan half-width |
| `sync_frames` | Frames to wait after each lens move before the first ISP sample |
| `metric_avg_frames` | After that wait, number of consecutive-frame ISP reads averaged per sample (reduces curve jitter) |
| `max_total_moves` | Safety cap on focus probes per run |
| `metric_min_luma` | Skip windows whose `luma` is below this |
| `metric_ratio_cap` | If **> 0**, cap per-window `sum/(luma+1)` (creates a flat plateau if the true peak exceeds the cap). **`0` = off (default).** |
| `early_stop_min_spread` | Minimum focus spread (pulses) of merged samples before quadratic early-stop; capped by current scan interval width; **`0` = disable** |
| `fit_min_samples` | Minimum merged distinct positions before quadratic early-stop is considered (default **8**) |
| `fit_min_curvature` | Minimum \|a\| for accepting a downward-opening quadratic fit |

Use `af_param_get` for current values; `print_help` lists the `af_param_set` keys.

## OSD

- **AF windows**: rectangles over the encoder picture, scaled from video coordinates to codec resolution when they differ.
- **Focus curve**: after each successful `af_run_once`, if `af_osd 1`, a **~800×400** ARGB strip (clamped to the encoder size) is drawn in the **top-right** (small pixel inset). It plots merged coarse+fine samples sorted by focus position.
- Disabling `af_osd` hides both window outlines and the curve overlay.

## CLI commands (summary)

- **Streams / versions**: `streams`, `versions`, `vidx`, `cidx`
- **ISP AF**: `isp_af_set`, `isp_af_get`, `isp_af_meas`
- **MCU / lens**: `mcu_init`, `mcu_deinit`, `af_create`, `af_destroy`, `zoom_goto`
- **AF engine**: `af_param_get`, `af_param_set`, `af_run_once`, `af_stop`, `af_track`, `af_status`
- **OSD**: `af_osd`, `af_osd_style`
- **UDP push**: `udp_start`, `udp_stop`, `udp_select|udp_push`, `udp_stats`

## Notes

- OSD custom overlay pixel data is written into the MediaLibrary custom overlay buffer (see `osd_privacy_example` under the MediaLibrary API examples in this repo for the same pattern).
- If your encoder resolution differs from the video stream resolution, AF windows are scaled before drawing.
- If video `subscribe_stream` is unavailable, frame waits fall back to an FPS-derived sleep; tune `sync_frames` / `metric_avg_frames` for your pipeline latency.
