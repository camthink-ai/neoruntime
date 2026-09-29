/**
 * @file daynight_policy.h
 * @brief Pure light-sensor day/night switching decision logic (no HAL dependencies).
 *
 * The camera-daemon wiring owns the hardware side-effects (it calls the existing
 * atomic CameraDaemon::set_imaging_mode()). This module only turns a stream of
 * photodiode readings into a switching decision, applying hysteresis and
 * sample-stability debouncing so the optical state does not chatter near the
 * threshold boundary, and deferring a confirmed switch while a lens/AF operation
 * is in progress.
 *
 * Ported from the auto_af_test demo's af_daynight_policy (endpoint-calibrated).
 */

#pragma once

#include <cstdint>
#include <string>

/** Active optical state (what the camera is currently configured for). */
enum class LightMode
{
    Day,
    Night
};

struct LightSensorConfig
{
    bool enabled = true;
    bool auto_on_boot = false;       /* start in auto mode on boot */
    int night_enter = 25;           /* light_percent <= night_enter => Night (25% ≈ 11.68 lux, bench) */
    int day_enter = 80;             /* light_percent >= day_enter   => Day   (80% ≈ 25.36 lux, bench) */
    int sample_interval_ms = 500;   /* sampling cadence of the auto monitor */
    int stable_samples = 3;         /* consecutive qualifying reads before a switch */
    int min_hold_ms = 15000;        /* anti-flap dwell: no new switch within this window after an applied one */
    int dark_mv = 4;                /* measured dark-environment endpoint (calibration) */
    int bright_mv = 1909;           /* measured bright-environment endpoint (calibration) */
};

/** One normalized light reading plus the raw ADC values (kept for logging). */
struct LightSample
{
    bool valid = false;
    int percent = 0;       /* 0..100, endpoint-normalized from mv */
    uint16_t mv = 0;       /* raw milli-volts from the photodiode */
    int32_t milli = 0;     /* raw scaled value (platform-defined, e.g. lux*1000) */
};

/** What evaluate() asks the caller to do this tick. */
enum class LightSwitchDecision
{
    None,         /* nothing to do */
    ToDay,        /* confirmed transition to day */
    ToNight,      /* confirmed transition to night */
    DeferPending, /* confirmed but a lens/AF op is in progress; saved as pending */
    Held          /* confirmed but suppressed by the min_hold_ms anti-flap dwell */
};

/** Persistent policy scratch updated by evaluate(). */
struct DayNightPolicyState
{
    LightMode mode = LightMode::Day;            /* current optical state */
    int stable_count = 0;                       /* consecutive reads toward one target */
    LightMode accum_target = LightMode::Day;    /* target currently being accumulated */
    bool has_pending = false;                   /* confirmed switch deferred due to lens/AF op */
    LightMode pending_target = LightMode::Day;
    uint64_t last_switch_ms = 0;                /* steady-clock ms of the last APPLIED switch; 0 = none yet */
    LightSample last;
};

/**
 * @brief Endpoint-calibrated light percent normalization.
 *
 * percent = clamp(round(100 * (mv - dark_mv) / (bright_mv - dark_mv)), 0, 100).
 */
int normalize_light_percent(uint16_t mv, int32_t milli, const LightSensorConfig &config);

/** Validate a night_enter/day_enter pair: both in [0,100] and night_enter < day_enter. */
bool validate_light_thresholds(int night_enter, int day_enter, std::string *error = nullptr);

const char *light_mode_name(LightMode mode);

/**
 * @brief Advance the policy with one light reading.
 *
 * Hysteresis (night_enter / day_enter) gates a candidate target; only
 * `stable_samples` consecutive reads in the same direction confirm it. If a
 * switch is confirmed while `lens_op_active` is true, the switch is saved as
 * pending (DeferPending) instead of applied, so it does not interrupt a
 * lens/AF operation.
 *
 * Anti-flap dwell: a confirmed switch is suppressed (Held, state unchanged,
 * accumulation kept) while now_ms is within min_hold_ms of the last APPLIED
 * switch, so light hovering across a threshold — or the IR-LED feedback it
 * triggers — cannot chatter the optical state. It fires on the next tick
 * after the dwell expires (still light-confirmed). Pass now_ms as
 * steady-clock milliseconds since epoch; the default 0 disables the dwell
 * for tests/legacy callers.
 *
 * On a ToDay/ToNight return the policy's `mode` has already been updated,
 * `stable_count` reset and `last_switch_ms` armed; the caller performs the
 * hardware side-effects.
 */
LightSwitchDecision evaluate(DayNightPolicyState &policy, const LightSample &sample,
                             const LightSensorConfig &config, bool lens_op_active,
                             uint64_t now_ms = 0);
