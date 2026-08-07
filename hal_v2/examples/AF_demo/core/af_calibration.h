#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hal_auto_af
{

struct CalibrationPoint
{
    float zoom_ratio{1.0f};
    int32_t table_focus{0};
    int32_t best_focus{0};
    int32_t delta_focus{0};
    double confidence{0.0};
    bool accepted{false};
};

struct CalibrationProfile
{
    bool loaded{false};
    float distance_m{0.0f};
    std::string path{};
    std::vector<CalibrationPoint> points{};
};

enum class ZoomDirection
{
    WideToTele,
    TeleToWide,
};

const char *zoom_direction_name(ZoomDirection direction);
bool parse_zoom_direction(const char *text, ZoomDirection *out_direction);

struct CalibrationV2Point
{
    float zoom_ratio{1.0f};
    float distance_m{0.0f};
    double diopter{0.0};
    ZoomDirection zoom_direction{ZoomDirection::WideToTele};
    int32_t table_focus{0};
    int32_t best_focus{0};
    int32_t delta_focus{0}; /* best_focus - table_focus; runtime adds this delta */
    double sigma_focus{0.0};
    int sample_count{0};
    double confidence{0.0};
    bool accepted{false};
};

struct CalibrationProfileV2
{
    bool loaded{false};
    std::string path{};
    std::vector<CalibrationV2Point> points{};
};

struct CalibrationRepeat
{
    int32_t best_focus{0};
    double confidence{0.0};
    bool accepted{false};
};

bool aggregate_calibration_v2_point(float zoom_ratio, float distance_m, ZoomDirection direction,
                                    int32_t table_focus, const std::vector<CalibrationRepeat> &repeats,
                                    CalibrationV2Point *out_point);
void upsert_calibration_v2_point(CalibrationProfileV2 *profile, const CalibrationV2Point &point);
bool calibration_v2_lookup(const CalibrationProfileV2 &profile, float ratio, float distance_m,
                           ZoomDirection direction, double strength, int32_t *out_delta,
                           double *out_sigma);
bool save_calibration_v2_file(const char *path, const CalibrationProfileV2 &profile);
bool load_calibration_v2_file(const char *path, CalibrationProfileV2 *out_profile);

bool calibration_delta_for_ratio(const CalibrationProfile &profile, float ratio,
                                 bool use_conservative_bias, double strength, int32_t *out_delta);
bool save_calibration_file(const char *path, const CalibrationProfile &profile);
bool load_calibration_file(const char *path, CalibrationProfile *out_profile);

} // namespace hal_auto_af
