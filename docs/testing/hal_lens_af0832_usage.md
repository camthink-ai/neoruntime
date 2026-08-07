# AF0832 Lens Driver (`hal_lens_af0832`) Usage Guide

This document is intended for platform/application-side callers, explaining how to use the AF0832 convenience wrapper API provided by `hal_v2/include/peripheral/devices/hal_lens_af0832.h` to control the lens **zoom**, **focus**, and **iris**.

> Implementation is located at `hal_v2/common/devices/hal_lens_af0832.c`, which communicates with the MCU through `HAL_LENS_OPS` from `hal_v2/include/peripheral/devices/hal_lens.h`.

## 1. Component Relationships and Dependencies

- **`hal_lens_af0832`**: High-level convenience API for business use, encapsulating initialization/configuration/reset-zero/wait-for-completion-event flows.
- **`HAL_LENS_OPS` (`HalLensOps`)**: Low-level lens primitive interface (executes motor actions, queries status, subscribes to events via MCU).
- **`mcu_ctx`**: MCU communication context pointer provided by the platform layer; `hal_lens_af0832_*` calls `HAL_LENS_OPS.*` through it.

## 2. Key Types and Parameter Meanings

### 2.1 `HalLensMotion`

Used for `*_run` / `*_abs` motion control:

- `pps`: Speed, in **pulses per second**
- `value`:
  - `*_run`: Relative motion amount (commented as micro steps, can be understood as "how many steps/micro-steps to move")
  - `*_abs`: Target **absolute position** (position)

### 2.2 `HalLensLimit`

Soft limit range (absolute position units same as `HalLensState.zoom_pos/focus_pos`):

- `min_pos`: Minimum allowed position
- `max_pos`: Maximum allowed position

### 2.3 `HalLensState`

Status query structure:

- `zoom_pos` / `focus_pos`: Current absolute position
- `zoom_rz_done` / `focus_rz_done`: Whether reset-zero has been completed
- `zoom_state` / `focus_state` / `iris_state`: Motor status codes (aligned with MCU wire values, see below)

## 3. Position/Step Units and Specification Table Conversion (Important)

The specification table (Tracking curve table) you provided states:

- The "steps" in the table are the lens design step units
- **1 step on the table = 4 steps on our HAL side**

Therefore, this document consistently uses the following conversion relationship (for mapping specification table data to `HalLensState.zoom_pos/focus_pos` and the absolute position meaning of `HalLensMotion.value`):

- `hal_steps = table_steps * 4`
- `table_steps = hal_steps / 4`

> Note: The `HAL_LENS_AF0832_DEFAULT_*` values in `hal_lens_af0832.h` are marked as placeholders in the header comments. It is recommended to set `HalLensAf0832Params.zoom_limit/focus_limit` based on calibration data; the sections below provide "specification table to HAL units" mappings that can be directly applied.

## 4. Specification Table Mapping: Magnification vs. ZOOM Position (W to T)

Below is an excerpt from the specification table "Zoom Steps (W to T)", converted to HAL steps (x4).

> Note: Your current model's **zoom direction is opposite to the specification table**, so the `zoom_pos` used for `af_goto` takes a **negative sign** (i.e., `hal_zoom_pos = -(table_steps * 4)`).

```text
Magnification    table_steps(W->T)   hal_zoom_pos(=-(table*4))
1.00    799               -3196
1.20    609               -2436
1.40    454               -1816
1.60    326               -1304
1.80    217               -868
2.00    122               -488
2.20    38                -152
2.40    -36               144
2.60    -101              404
2.80    -159              636
2.88    -180              720
```

## 5. Specification Table Mapping: Distance vs. FOCUS Position (Far to Near)

Below is an excerpt from the specification table "Focus Steps (Far to Near)", converted to HAL steps (x4).

> The values below are the steps from Far to the specified distance (including INF/10m/.../1.5m) at "different magnifications"; when calling `focus_abs`, use the converted `hal_steps` as the target position reference (calibration/actual measurement fine-tuning is still recommended in practice).

```text
Mag  1.00: INF -66(-264)  10m -68(-272)  5m -69(-276)  3m -70(-280)  2.5m -71(-284)  2m -73(-292)  1.5m -74(-296)
Mag  1.20: INF -100(-400) 10m -102(-408) 5m -103(-412) 3m -106(-424) 2.5m -107(-428) 2m -108(-432) 1.5m -111(-444)
Mag  1.40: INF -119(-476) 10m -122(-488) 5m -124(-496) 3m -127(-508) 2.5m -128(-512) 2m -131(-524) 1.5m -134(-536)
Mag  1.60: INF -126(-504) 10m -129(-516) 5m -132(-528) 3m -135(-540) 2.5m -137(-548) 2m -140(-560) 1.5m -145(-580)
Mag  1.80: INF -121(-484) 10m -125(-500) 5m -129(-516) 3m -134(-536) 2.5m -136(-544) 2m -140(-560) 1.5m -146(-584)
Mag  2.00: INF -106(-424) 10m -111(-444) 5m -115(-460) 3m -122(-488) 2.5m -125(-500) 2m -129(-516) 1.5m -137(-548)
Mag  2.20: INF -81(-324)  10m -87(-348)  5m -92(-368)  3m -100(-400) 2.5m -104(-416) 2m -109(-436) 1.5m -118(-472)
Mag  2.40: INF -46(-184)  10m -53(-212)  5m -60(-240)  3m -69(-276)  2.5m -74(-296)  2m -80(-320)  1.5m -91(-364)
Mag  2.60: INF -2(-8)     10m -11(-44)   5m -19(-76)   3m -30(-120)  2.5m -35(-140)  2m -44(-176) 1.5m -57(-228)
Mag  2.80: INF 51(204)    10m 40(160)    5m 30(120)    3m 17(68)     2.5m 10(40)     2m 1(4)       1.5m -15(-60)
Mag  2.88: INF 75(300)    10m 63(252)    5m 52(208)    3m 38(152)    2.5m 31(124)    2m 20(80)     1.5m 3(12)
```

## 6. Mechanical Dead Points (Specification Table Excerpt)

The specification table also provides mechanical dead points (also convertible to HAL steps using x4):

```text
ZOOM mechanical front dead point steps:  809  (HAL: -3236, reversed)
ZOOM mechanical rear dead point steps:  -190 (HAL: 760, reversed)
FOCUS mechanical rear dead point steps: -211 (HAL: -844)
FOCUS mechanical front dead point steps:  148 (HAL: 592)
```

> Tip: The default `zoom_limit/focus_limit` in `HalLensAf0832Params` has been set to a usable range based on the above mechanical dead point conversions, ensuring that `af_goto` (magnification + distance mapping) can cover the table range of 1.0x to 2.88x and INF to 1.5m even without explicitly configured limits.

> Note: The example mapping/excerpt above is from an earlier specification table screenshot; actual `af_goto` interpolation data is based on the built-in tracking table in code (generated from `.xls` to `.inc`).

## 7. Recommended Call Sequence (Must Follow This Order)

### 7.1 Creation and Parameter Initialization

1. Initialize parameters and override limits/timeouts with calibration values:
   - `hal_lens_af0832_params_init_defaults(&params)`
   - Modify:
     - `params.zoom_limit` / `params.focus_limit` (strongly recommended to replace with calibration values)
     - `params.bootstrap_timeout_ms` (reset-zero/bootstrap timeout)
     - `params.op_timeout_ms` (timeout for waiting completion of each zoom/focus operation)
2. `hal_lens_af0832_create(mcu_ctx, &params, &dev)`

### 7.2 Event Callback (Recommended)

- `hal_lens_af0832_set_event_callback(dev, cb, userdata)`
- Recommended to set before `bootstrap` so that reset-zero process events are also received.

### 7.3 Bootstrap (Required)

- `hal_lens_af0832_bootstrap(dev)`

This step internally:

- Subscribes to MCU lens events (for subsequent synchronous waiting)
- Decides whether `lens_init` is needed based on state
- Executes `lens_config(HAL_LENS_MODE_ALL)`
- **Sets zoom/focus soft limits first** (to avoid MCU rejecting reset-zero due to unconfigured limits)
- Performs zoom/focus reset-zero when necessary, and waits for completion (via event or polling `*_rz_done`)

> After `bootstrap` succeeds, `dev` is marked as initialized, and subsequent calls to `state_get/zoom/focus/iris` will be allowed.

### 7.4 Forced Reset-Zero (Optional)

- `hal_lens_af0832_force_reset_zero(dev)`
  - Forces **zoom_rz + focus_rz**, blocking until both axes complete (either event or `*_rz_done` polling satisfies the condition)
  - Applicable scenarios: on-site state is unreliable, need to re-zero to establish a consistent coordinate system

## 8. Control Interface Description

### 8.1 State Read

- `hal_lens_af0832_state_get(dev, &state)`

### 8.2 Zoom

- `hal_lens_af0832_zoom_run(dev, &motion)`
  - Relative movement
  - `motion.value == 0`: Returns success immediately (avoids waiting for a completion event that will never come)
  - Otherwise: Waits for `HAL_LENS_EVT_ZOOM_COMPLETED`, timeout uses `params.op_timeout_ms`

- `hal_lens_af0832_zoom_abs(dev, &motion)`
  - Absolute positioning
  - If current `zoom_pos == motion.value`: Returns success immediately (avoids waiting for a completion event that won't fire)
  - Otherwise: Waits for `HAL_LENS_EVT_ZOOM_COMPLETED`

### 8.3 Focus

- `hal_lens_af0832_focus_run(dev, &motion)`
  - Relative movement
  - `motion.value == 0`: Returns success immediately
  - Otherwise: Waits for `HAL_LENS_EVT_FOCUS_COMPLETED`

- `hal_lens_af0832_focus_abs(dev, &motion)`
  - Absolute positioning
  - If current `focus_pos == motion.value`: Returns success immediately
  - Otherwise: Waits for `HAL_LENS_EVT_FOCUS_COMPLETED`

### 8.4 Iris

- `hal_lens_af0832_iris_run(dev, &motion)`
  - There is currently **no iris completion bit** in the MCU event set; this interface does not wait for completion events and only uses the underlying RPC return value
- `hal_lens_af0832_iris_target_set(dev, target)`
- `hal_lens_af0832_iris_adc_get(dev, &adc)`

### 8.5 Magnification + Distance Auto Action (Recommended)

- `hal_lens_af0832_goto_by_ratio_distance(dev, zoom_ratio, focus_distance_m)`
  - Input optical magnification and focus distance (meters); internally interpolates based on specification table and executes:
    - `zoom_abs` (wait for completion) -> `focus_abs` (wait for completion)
  - Step conversion: **1 step on table = 4 steps on HAL**
  - Parameter rules:
    - `zoom_ratio`: Clamped to **current built-in tracking table min/max magnification**, linearly interpolated between adjacent magnification rows
    - `focus_distance_m`:
      - `<= 0`: Treated as INF
      - Otherwise: Clamped to [1.5, 10.0] meters, linearly interpolated between adjacent distance columns
    - `pps`: Uses `HalLensAf0832Params.default_pps`

  - Tracking table data source (560nm):
    - Generated from `hal_v2/doc/AF0832DB.ICR Focus Curve-20190819.xls`
    - Three `.inc` files are currently stored in the repository (located at `hal_v2/common/devices/`):
      - `hal_lens_af0832_table_560nm_base.inc` (base back focus)
      - `hal_lens_af0832_table_560nm_bf_p01.inc` (back focus +0.1mm)
      - `hal_lens_af0832_table_560nm_bf_m01.inc` (back focus -0.1mm)
    - **Current default: back focus -0.1mm** (`hal_lens_af0832_table_560nm_bf_m01.inc`)

## 9. Event Bits and Status Codes

### 9.1 Event Bits (callback `event`)

- `HAL_LENS_EVT_ZOOM_COMPLETED`: Zoom completed
- `HAL_LENS_EVT_FOCUS_COMPLETED`: Focus completed
- `HAL_LENS_EVT_ZOOM_RESET_DONE`: Zoom reset-zero completed
- `HAL_LENS_EVT_FOCUS_RESET_DONE`: Focus reset-zero completed

### 9.2 Motor Status Codes (`HalLensState.*_state`)

Consistent with MCU wire values (for interpreting `zoom_state/focus_state/iris_state`):

- `HAL_LENS_MS41908M_STATE_NO_CFG`: Not configured
- `HAL_LENS_MS41908M_STATE_STOPPED`: Stopped
- `HAL_LENS_MS41908M_STATE_RUNNING`: Running
- `HAL_LENS_MS41908M_STATE_RESET_ZERO`: Resetting to zero
- `HAL_LENS_MS41908M_STATE_ERROR`: Error

## 10. Return Codes and Timeout Handling Recommendations

All interfaces return `HAL_OK (0)` for success; common error codes:

- `HAL_ERR_INVALID_ARG`: Invalid parameter
- `HAL_ERR_NOT_INITIALIZED`: Not bootstrapped
- `HAL_ERR_TIMEOUT`: Timeout waiting for completion event (zoom/focus operation or bootstrap reset-zero)
- `HAL_ERR_INVALID_STATE`: Wait interrupted during destroy/close process
- `HAL_ERROR`: Underlying layer returned non-zero or internal error

Handling recommendations:

- After timeout, first call `hal_lens_af0832_state_get` to check if already at target position / `*_rz_done`, then decide whether to retry, reduce speed (pps), or re-bootstrap.

## 11. Minimal Usage Example (Pseudocode)

```c
HalLensAf0832 *dev = NULL;
HalLensAf0832Params p;
hal_lens_af0832_params_init_defaults(&p);

// Recommended: Replace default limits with calibration values
p.zoom_limit.min_pos = HAL_LENS_AF0832_DEFAULT_ZOOM_MIN_POS;
p.zoom_limit.max_pos = HAL_LENS_AF0832_DEFAULT_ZOOM_MAX_POS;
p.focus_limit.min_pos = HAL_LENS_AF0832_DEFAULT_FOCUS_MIN_POS;
p.focus_limit.max_pos = HAL_LENS_AF0832_DEFAULT_FOCUS_MAX_POS;

// Create
hal_lens_af0832_create(mcu_ctx, &p, &dev);

// Optional: Event callback
hal_lens_af0832_set_event_callback(dev, my_cb, my_ud);

// Bootstrap (configure + reset-zero if needed and wait)
hal_lens_af0832_bootstrap(dev);

// Auto action via "magnification + distance" (per specification table mapping, table steps x4)
hal_lens_af0832_goto_by_ratio_distance(dev, 2.0f, 3.0f);  // 2.0x, 3m

// Zoom to absolute position (example)
HalLensMotion z = {.pps = 1200, .value = 100};
hal_lens_af0832_zoom_abs(dev, &z);

// Focus relative movement (example)
HalLensMotion f = {.pps = 800, .value = -50};
hal_lens_af0832_focus_run(dev, &f);

// Destroy
hal_lens_af0832_destroy(dev);
```
