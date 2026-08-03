#include "af_ir_follow.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{

void check(bool condition, const char *message)
{
    if (!condition)
    {
        std::fprintf(stderr, "af-ir-follow-test: %s\n", message);
        std::exit(1);
    }
}

void test_default_lut_and_interpolation()
{
    const auto lut = hal_auto_af::default_ir_zoom_lut();
    std::string error;
    bool warning = false;
    check(hal_auto_af::validate_ir_zoom_lut(lut, &error, &warning),
          "default LUT validation failed");
    check(!warning, "default LUT unexpectedly raised a monotonic warning");

    const auto low = hal_auto_af::interpolate_ir_zoom_lut(lut, 0.5);
    check(low.near_pwm == 100 && low.far_pwm == 0, "low clamp is wrong");

    const auto middle = hal_auto_af::interpolate_ir_zoom_lut(lut, 2.10);
    check(middle.near_pwm == 67 && middle.far_pwm == 16,
          "2.10x interpolation is wrong");

    const auto high = hal_auto_af::interpolate_ir_zoom_lut(lut, 4.0);
    check(high.near_pwm == 0 && high.far_pwm == 100, "high clamp is wrong");
}

void test_priority_and_queued_manual()
{
    hal_auto_af::IrFollowState state{};
    state.lut = hal_auto_af::default_ir_zoom_lut();

    auto desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 0 && desired.far_pwm == 0, "day mode did not force IR off");

    state.night_mode = true;
    desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 75 && desired.far_pwm == 10, "idle LUT output is wrong");

    hal_auto_af::set_ir_manual(&state, {40, 30});
    desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 40 && desired.far_pwm == 30,
          "manual output did not override the idle LUT");

    hal_auto_af::begin_ir_follow(&state);
    hal_auto_af::set_ir_manual(&state, {20, 60});
    desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 75 && desired.far_pwm == 10,
          "manual update changed output during follow");

    hal_auto_af::end_ir_follow(&state);
    desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 20 && desired.far_pwm == 60,
          "queued manual output was not restored");

    hal_auto_af::clear_ir_manual(&state);
    desired = hal_auto_af::desired_ir_pwm(state, 2.0);
    check(desired.near_pwm == 75 && desired.far_pwm == 10,
          "clearing manual did not restore the idle LUT");
}

void test_validation_deadband_and_csv()
{
    std::string error;
    std::vector<hal_auto_af::IrLutPoint> invalid = {
        {1.0, 100, 0},
        {1.0, 80, 20},
    };
    check(!hal_auto_af::validate_ir_zoom_lut(invalid, &error),
          "duplicate zoom ratios were accepted");

    check(!hal_auto_af::ir_pwm_channel_needs_update(50, 51, 2),
          "deadband did not suppress a one-percent change");
    check(hal_auto_af::ir_pwm_channel_needs_update(50, 52, 2),
          "deadband suppressed a two-percent change");

    auto lut = hal_auto_af::default_ir_zoom_lut();
    check(hal_auto_af::upsert_ir_zoom_lut_point(
              &lut, hal_auto_af::IrLutPoint{2.10, 66, 17}, &error),
          "inserting a runtime LUT point failed");
    const auto inserted = hal_auto_af::interpolate_ir_zoom_lut(lut, 2.10);
    check(inserted.near_pwm == 66 && inserted.far_pwm == 17,
          "inserted LUT point was not used");

    const char *path = "/tmp/ne503-af-ir-follow-test.csv";
    check(hal_auto_af::save_ir_zoom_lut_csv(path, lut, &error), "saving LUT CSV failed");
    std::vector<hal_auto_af::IrLutPoint> loaded;
    bool warning = false;
    check(hal_auto_af::load_ir_zoom_lut_csv(path, &loaded, &error, &warning),
          "loading LUT CSV failed");
    check(loaded.size() == lut.size(), "CSV round trip changed point count");
    check(std::fabs(loaded[4].zoom_ratio - lut[4].zoom_ratio) < 1e-9,
          "CSV round trip changed a zoom ratio");
    std::remove(path);

    std::vector<hal_auto_af::IrLutPoint> experimental = {
        {1.0, 50, 50},
        {2.0, 60, 40},
    };
    warning = false;
    check(hal_auto_af::validate_ir_zoom_lut(experimental, &error, &warning),
          "experimental non-monotonic LUT should remain valid");
    check(warning, "experimental non-monotonic LUT did not raise a warning");
}

} // namespace

int main()
{
    test_default_lut_and_interpolation();
    test_priority_and_queued_manual();
    test_validation_deadband_and_csv();
    std::puts("af-ir-follow-test: all tests passed");
    return 0;
}
