#include "af_ir_follow.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace hal_auto_af
{

namespace
{

std::string trim(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
    {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

bool parse_double(const std::string &text, double *value)
{
    if (!value)
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const double parsed = std::strtod(text.c_str(), &end);
    if (errno != 0 || end == text.c_str() || *end != '\0' || !std::isfinite(parsed))
    {
        return false;
    }
    *value = parsed;
    return true;
}

bool parse_int(const std::string &text, int *value)
{
    if (!value)
    {
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const long parsed = std::strtol(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str() || *end != '\0')
    {
        return false;
    }
    *value = static_cast<int>(parsed);
    return true;
}

} // namespace

std::vector<IrLutPoint> default_ir_zoom_lut()
{
    return {
        {1.00, 100, 0},
        {1.25, 85, 0},
        {1.50, 80, 0},
        {1.75, 80, 5},
        {2.00, 75, 10},
        {2.25, 55, 25},
        {2.50, 32, 50},
        {2.75, 12, 80},
        {2.88, 0, 100},
    };
}

bool validate_ir_zoom_lut(const std::vector<IrLutPoint> &lut, std::string *error,
                          bool *monotonic_warning)
{
    if (monotonic_warning)
    {
        *monotonic_warning = false;
    }
    if (lut.empty())
    {
        if (error)
        {
            *error = "LUT is empty";
        }
        return false;
    }

    for (size_t i = 0; i < lut.size(); ++i)
    {
        const auto &point = lut[i];
        if (!std::isfinite(point.zoom_ratio) || point.zoom_ratio <= 0.0)
        {
            if (error)
            {
                *error = "zoom ratio must be finite and positive";
            }
            return false;
        }
        if (point.near_pwm < 0 || point.near_pwm > 100 ||
            point.far_pwm < 0 || point.far_pwm > 100)
        {
            if (error)
            {
                *error = "PWM values must be in [0,100]";
            }
            return false;
        }
        if (i == 0)
        {
            continue;
        }
        const auto &previous = lut[i - 1];
        if (point.zoom_ratio <= previous.zoom_ratio)
        {
            if (error)
            {
                *error = "zoom ratios must be strictly increasing";
            }
            return false;
        }
        if (monotonic_warning &&
            (point.near_pwm > previous.near_pwm || point.far_pwm < previous.far_pwm))
        {
            *monotonic_warning = true;
        }
    }
    return true;
}

IrPwm interpolate_ir_zoom_lut(const std::vector<IrLutPoint> &lut, double zoom_ratio)
{
    if (lut.empty())
    {
        return {};
    }
    if (zoom_ratio <= lut.front().zoom_ratio)
    {
        return {lut.front().near_pwm, lut.front().far_pwm};
    }
    if (zoom_ratio >= lut.back().zoom_ratio)
    {
        return {lut.back().near_pwm, lut.back().far_pwm};
    }

    const auto upper = std::upper_bound(
        lut.begin(), lut.end(), zoom_ratio,
        [](double ratio, const IrLutPoint &point) { return ratio < point.zoom_ratio; });
    const auto &right = *upper;
    const auto &left = *(upper - 1);
    const double denominator = right.zoom_ratio - left.zoom_ratio;
    const double t = denominator > 0.0 ? (zoom_ratio - left.zoom_ratio) / denominator : 0.0;
    const int near_pwm = static_cast<int>(std::lround(
        static_cast<double>(left.near_pwm) +
        t * static_cast<double>(right.near_pwm - left.near_pwm)));
    const int far_pwm = static_cast<int>(std::lround(
        static_cast<double>(left.far_pwm) +
        t * static_cast<double>(right.far_pwm - left.far_pwm)));
    return {std::clamp(near_pwm, 0, 100), std::clamp(far_pwm, 0, 100)};
}

bool upsert_ir_zoom_lut_point(std::vector<IrLutPoint> *lut, const IrLutPoint &point,
                              std::string *error)
{
    if (!lut)
    {
        if (error)
        {
            *error = "null LUT";
        }
        return false;
    }
    if (!std::isfinite(point.zoom_ratio) || point.zoom_ratio <= 0.0 ||
        point.near_pwm < 0 || point.near_pwm > 100 ||
        point.far_pwm < 0 || point.far_pwm > 100)
    {
        if (error)
        {
            *error = "invalid LUT point";
        }
        return false;
    }

    constexpr double kRatioEpsilon = 1e-6;
    auto it = std::find_if(lut->begin(), lut->end(), [&](const IrLutPoint &existing) {
        return std::fabs(existing.zoom_ratio - point.zoom_ratio) <= kRatioEpsilon;
    });
    if (it != lut->end())
    {
        *it = point;
    }
    else
    {
        lut->push_back(point);
    }
    std::sort(lut->begin(), lut->end(),
              [](const IrLutPoint &a, const IrLutPoint &b) {
                  return a.zoom_ratio < b.zoom_ratio;
              });
    return validate_ir_zoom_lut(*lut, error);
}

bool load_ir_zoom_lut_csv(const std::string &path, std::vector<IrLutPoint> *lut,
                          std::string *error, bool *monotonic_warning)
{
    if (!lut)
    {
        if (error)
        {
            *error = "null LUT";
        }
        return false;
    }
    std::ifstream input(path);
    if (!input)
    {
        if (error)
        {
            *error = "cannot open " + path;
        }
        return false;
    }

    std::vector<IrLutPoint> parsed;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line))
    {
        ++line_number;
        line = trim(line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        std::stringstream row(line);
        std::string ratio_text;
        std::string near_text;
        std::string far_text;
        std::string extra;
        if (!std::getline(row, ratio_text, ',') ||
            !std::getline(row, near_text, ',') ||
            !std::getline(row, far_text, ',') ||
            std::getline(row, extra, ','))
        {
            if (error)
            {
                *error = "invalid CSV column count at line " + std::to_string(line_number);
            }
            return false;
        }

        ratio_text = trim(ratio_text);
        near_text = trim(near_text);
        far_text = trim(far_text);
        if (parsed.empty() && ratio_text == "zoom_ratio")
        {
            continue;
        }

        IrLutPoint point{};
        if (!parse_double(ratio_text, &point.zoom_ratio) ||
            !parse_int(near_text, &point.near_pwm) ||
            !parse_int(far_text, &point.far_pwm))
        {
            if (error)
            {
                *error = "invalid CSV value at line " + std::to_string(line_number);
            }
            return false;
        }
        parsed.push_back(point);
    }

    if (!validate_ir_zoom_lut(parsed, error, monotonic_warning))
    {
        return false;
    }
    *lut = std::move(parsed);
    return true;
}

bool save_ir_zoom_lut_csv(const std::string &path, const std::vector<IrLutPoint> &lut,
                          std::string *error)
{
    if (!validate_ir_zoom_lut(lut, error))
    {
        return false;
    }
    std::ofstream output(path);
    if (!output)
    {
        if (error)
        {
            *error = "cannot create " + path;
        }
        return false;
    }
    output << "zoom_ratio,near_pwm,far_pwm\n";
    output << std::fixed << std::setprecision(3);
    for (const auto &point : lut)
    {
        output << point.zoom_ratio << ',' << point.near_pwm << ',' << point.far_pwm << '\n';
    }
    if (!output)
    {
        if (error)
        {
            *error = "failed writing " + path;
        }
        return false;
    }
    return true;
}

IrPwm desired_ir_pwm(const IrFollowState &state, double zoom_ratio)
{
    if (!state.night_mode)
    {
        return {};
    }
    if (state.follow_active && state.config.auto_follow)
    {
        return interpolate_ir_zoom_lut(state.lut, zoom_ratio);
    }
    if (state.manual_valid)
    {
        return state.manual;
    }
    if (state.config.auto_follow)
    {
        return interpolate_ir_zoom_lut(state.lut, zoom_ratio);
    }
    return {};
}

bool ir_pwm_channel_needs_update(int current_pwm, int target_pwm, int deadband)
{
    return std::abs(current_pwm - target_pwm) >= std::max(0, deadband);
}

void set_ir_manual(IrFollowState *state, IrPwm pwm)
{
    if (!state)
    {
        return;
    }
    state->manual.near_pwm = std::clamp(pwm.near_pwm, 0, 100);
    state->manual.far_pwm = std::clamp(pwm.far_pwm, 0, 100);
    state->manual_valid = true;
}

void clear_ir_manual(IrFollowState *state)
{
    if (!state)
    {
        return;
    }
    state->manual_valid = false;
}

void begin_ir_follow(IrFollowState *state)
{
    if (state)
    {
        state->follow_active = true;
        state->degraded = false;
    }
}

void end_ir_follow(IrFollowState *state)
{
    if (state)
    {
        state->follow_active = false;
    }
}

} // namespace hal_auto_af
