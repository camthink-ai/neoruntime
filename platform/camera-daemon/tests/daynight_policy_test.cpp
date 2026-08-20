#include "daynight_policy.h"

#include <cassert>
#include <string>

static LightSample make_sample(int percent) {
    LightSample s;
    s.valid = true;
    s.percent = percent;
    return s;
}

int main() {
    LightSensorConfig config;            // night_enter=28, day_enter=82, stable_samples=3, dark=4, bright=1909
    DayNightPolicyState state;           // mode=Day

    /* ---- normalize_light_percent (endpoint calibration) ---- */
    assert(normalize_light_percent(1909, 0, config) == 100);  // bright endpoint
    assert(normalize_light_percent(4, 0, config) == 0);       // dark endpoint
    assert(normalize_light_percent(0, 0, config) == 0);       // below dark clamps to 0
    assert(normalize_light_percent(5000, 0, config) == 100);  // above bright clamps to 100

    /* ---- validate_light_thresholds ---- */
    assert(validate_light_thresholds(28, 82));
    assert(!validate_light_thresholds(82, 28));
    assert(!validate_light_thresholds(-1, 82));
    assert(!validate_light_thresholds(28, 101));
    std::string err;
    assert(!validate_light_thresholds(50, 50, &err) && !err.empty());

    /* ---- evaluate: night transition needs stable_samples consecutive reads ---- */
    assert(evaluate(state, make_sample(10), config, false) == LightSwitchDecision::None);   // 1
    assert(evaluate(state, make_sample(10), config, false) == LightSwitchDecision::None);   // 2
    assert(evaluate(state, make_sample(10), config, false) == LightSwitchDecision::ToNight);// 3 -> Night
    assert(state.mode == LightMode::Night);

    /* ---- evaluate: day transition ---- */
    assert(evaluate(state, make_sample(90), config, false) == LightSwitchDecision::None);
    assert(evaluate(state, make_sample(90), config, false) == LightSwitchDecision::None);
    assert(evaluate(state, make_sample(90), config, false) == LightSwitchDecision::ToDay);  // -> Day
    assert(state.mode == LightMode::Day);

    /* ---- hysteresis band holds current state ---- */
    for (int i = 0; i < 5; ++i) {
        assert(evaluate(state, make_sample(50), config, false) == LightSwitchDecision::None);
    }
    assert(state.mode == LightMode::Day);

    /* ---- a candidate equal to current state resets accumulation ---- */
    assert(evaluate(state, make_sample(95), config, false) == LightSwitchDecision::None); // toward Day(=current)
    assert(state.stable_count == 0);

    /* ---- invalid sample resets accumulation ---- */
    assert(evaluate(state, make_sample(10), config, false) == LightSwitchDecision::None); // toward Night, count=1
    LightSample bad;
    bad.valid = false;
    assert(evaluate(state, bad, config, false) == LightSwitchDecision::None);
    assert(state.stable_count == 0);

    /* ---- zoom/lens-op active defers a confirmed switch ---- */
    state = DayNightPolicyState{};  // mode=Day
    assert(evaluate(state, make_sample(10), config, true) == LightSwitchDecision::None);
    assert(evaluate(state, make_sample(10), config, true) == LightSwitchDecision::None);
    assert(evaluate(state, make_sample(10), config, true) == LightSwitchDecision::DeferPending);
    assert(state.has_pending);
    assert(state.pending_target == LightMode::Night);
    assert(state.mode == LightMode::Day);  // not applied yet

    /* ---- light_mode_name ---- */
    assert(std::string(light_mode_name(LightMode::Day)) == "day");
    assert(std::string(light_mode_name(LightMode::Night)) == "night");

    return 0;
}
