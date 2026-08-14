#include "daynight_policy.h"

#include <algorithm>
#include <cmath>

int normalize_light_percent(uint16_t mv, int32_t milli, const LightSensorConfig &config)
{
    (void)milli; /* percent is calibrated from the measured voltage endpoints */
    const int dark_mv = std::max(0, config.dark_mv);
    const int bright_mv = std::max(dark_mv + 1, config.bright_mv);
    const double normalized =
        100.0 * (static_cast<double>(mv) - static_cast<double>(dark_mv)) /
        static_cast<double>(bright_mv - dark_mv);
    const int percent = static_cast<int>(std::lround(normalized));
    return std::clamp(percent, 0, 100);
}

bool validate_light_thresholds(int night_enter, int day_enter, std::string *error)
{
    if (night_enter < 0 || night_enter > 100 || day_enter < 0 || day_enter > 100)
    {
        if (error)
        {
            *error = "thresholds must be in [0,100]";
        }
        return false;
    }
    if (night_enter >= day_enter)
    {
        if (error)
        {
            *error = "night_enter must be strictly less than day_enter";
        }
        return false;
    }
    return true;
}

const char *light_mode_name(LightMode mode)
{
    return mode == LightMode::Night ? "night" : "day";
}

LightSwitchDecision evaluate(DayNightPolicyState &policy, const LightSample &sample,
                             const LightSensorConfig &config, bool lens_op_active)
{
    policy.last = sample;

    if (!sample.valid)
    {
        /* An unreadable sample resets accumulation so one bad read cannot flip state. */
        policy.stable_count = 0;
        return LightSwitchDecision::None;
    }

    /* Hysteresis: only the bands outside [night_enter, day_enter] propose a target. */
    LightMode candidate;
    bool has_candidate = false;
    if (sample.percent <= config.night_enter)
    {
        candidate = LightMode::Night;
        has_candidate = true;
    }
    else if (sample.percent >= config.day_enter)
    {
        candidate = LightMode::Day;
        has_candidate = true;
    }

    if (!has_candidate)
    {
        /* Inside the hysteresis band: hold the current state, forget pending reads. */
        policy.stable_count = 0;
        return LightSwitchDecision::None;
    }

    if (candidate == policy.mode)
    {
        /* Already in the target state; no transition needed. */
        policy.stable_count = 0;
        policy.has_pending = false;
        return LightSwitchDecision::None;
    }

    /* Accumulate consecutive reads toward the same candidate only. */
    if (candidate == policy.accum_target && policy.stable_count > 0)
    {
        ++policy.stable_count;
    }
    else
    {
        policy.accum_target = candidate;
        policy.stable_count = 1;
    }

    if (policy.stable_count < config.stable_samples)
    {
        return LightSwitchDecision::None;
    }

    /* Confirmed transition. */
    if (lens_op_active)
    {
        policy.has_pending = true;
        policy.pending_target = candidate;
        policy.stable_count = 0;
        return LightSwitchDecision::DeferPending;
    }

    policy.mode = candidate;
    policy.stable_count = 0;
    policy.has_pending = false;
    return candidate == LightMode::Day ? LightSwitchDecision::ToDay : LightSwitchDecision::ToNight;
}
