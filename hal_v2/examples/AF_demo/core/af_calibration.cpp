#include "af_calibration.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <map>
#include <numeric>
#include <string>
#include <utility>

namespace hal_auto_af
{
namespace
{

bool fixed_calibration_raw(float ratio, int32_t *out_delta)
{
    if (!out_delta)
    {
        return false;
    }
    /* Provisional 2.208 m fixed curve: midpoint of the measured W2T/T2W medians. */
    static constexpr std::array<std::pair<float, int32_t>, 9> bias = {{
        {1.00f, -24}, {1.25f, -32}, {1.50f, -36}, {1.75f, -44}, {2.00f, -68},
        {2.25f, -76}, {2.50f, -92}, {2.75f, -112}, {2.88f, -112},
    }};
    if (ratio <= bias.front().first)
    {
        *out_delta = bias.front().second;
        return true;
    }
    if (ratio >= bias.back().first)
    {
        *out_delta = bias.back().second;
        return true;
    }
    for (size_t i = 0; i + 1u < bias.size(); ++i)
    {
        const auto &a = bias[i];
        const auto &b = bias[i + 1u];
        if (ratio < a.first || ratio > b.first)
        {
            continue;
        }
        const float denominator = b.first - a.first;
        const float t = denominator > 1e-6f ? (ratio - a.first) / denominator : 0.0f;
        *out_delta = static_cast<int32_t>(std::lround(static_cast<double>(a.second) +
                                                      (static_cast<double>(b.second) - a.second) * t));
        return true;
    }
    return false;
}

bool profile_delta_raw(const CalibrationProfile &profile, float ratio, int32_t *out_delta)
{
    if (!out_delta || !profile.loaded)
    {
        return false;
    }
    std::vector<const CalibrationPoint *> points;
    for (const auto &point : profile.points)
    {
        if (point.accepted)
        {
            points.push_back(&point);
        }
    }
    if (points.empty())
    {
        return false;
    }
    std::sort(points.begin(), points.end(), [](const CalibrationPoint *a, const CalibrationPoint *b) {
        return a->zoom_ratio < b->zoom_ratio;
    });
    if (ratio < points.front()->zoom_ratio || ratio > points.back()->zoom_ratio)
    {
        return false;
    }
    for (size_t i = 0; i + 1u < points.size(); ++i)
    {
        const CalibrationPoint &a = *points[i];
        const CalibrationPoint &b = *points[i + 1u];
        if (ratio < a.zoom_ratio || ratio > b.zoom_ratio)
        {
            continue;
        }
        const float denominator = b.zoom_ratio - a.zoom_ratio;
        const float t = denominator > 1e-6f ? (ratio - a.zoom_ratio) / denominator : 0.0f;
        *out_delta = static_cast<int32_t>(std::lround(static_cast<double>(a.delta_focus) +
                                                      (static_cast<double>(b.delta_focus) - a.delta_focus) * t));
        return true;
    }
    return false;
}

template <typename T>
T median_value(std::vector<T> values)
{
    if (values.empty())
    {
        return T{};
    }
    std::sort(values.begin(), values.end());
    const size_t middle = values.size() / 2u;
    if ((values.size() & 1u) != 0u)
    {
        return values[middle];
    }
    return static_cast<T>((values[middle - 1u] + values[middle]) / static_cast<T>(2));
}

struct InterpolatedLayer
{
    double diopter{0.0};
    double delta{0.0};
    double sigma{0.0};
};

bool interpolate_zoom_layer(const std::vector<const CalibrationV2Point *> &points, float ratio,
                            InterpolatedLayer *out_layer)
{
    if (points.empty() || !out_layer)
    {
        return false;
    }
    std::vector<const CalibrationV2Point *> sorted = points;
    std::sort(sorted.begin(), sorted.end(), [](const CalibrationV2Point *a, const CalibrationV2Point *b) {
        return a->zoom_ratio < b->zoom_ratio;
    });
    const CalibrationV2Point *a = sorted.front();
    const CalibrationV2Point *b = sorted.front();
    if (ratio <= sorted.front()->zoom_ratio)
    {
        a = b = sorted.front();
    }
    else if (ratio >= sorted.back()->zoom_ratio)
    {
        a = b = sorted.back();
    }
    else
    {
        for (size_t i = 0; i + 1u < sorted.size(); ++i)
        {
            if (ratio >= sorted[i]->zoom_ratio && ratio <= sorted[i + 1u]->zoom_ratio)
            {
                a = sorted[i];
                b = sorted[i + 1u];
                break;
            }
        }
    }
    const double denominator = static_cast<double>(b->zoom_ratio - a->zoom_ratio);
    const double t = std::fabs(denominator) > 1e-9
                         ? std::clamp((static_cast<double>(ratio) - a->zoom_ratio) / denominator, 0.0, 1.0)
                         : 0.0;
    out_layer->diopter = a->diopter;
    out_layer->delta = static_cast<double>(a->delta_focus) +
                       (static_cast<double>(b->delta_focus - a->delta_focus) * t);
    out_layer->sigma = a->sigma_focus + (b->sigma_focus - a->sigma_focus) * t;
    return true;
}

} // namespace

const char *zoom_direction_name(ZoomDirection direction)
{
    return direction == ZoomDirection::TeleToWide ? "tele_to_wide" : "wide_to_tele";
}

bool parse_zoom_direction(const char *text, ZoomDirection *out_direction)
{
    if (!text || !out_direction)
    {
        return false;
    }
    const std::string value(text);
    if (value == "wide_to_tele" || value == "w2t" || value == "up")
    {
        *out_direction = ZoomDirection::WideToTele;
        return true;
    }
    if (value == "tele_to_wide" || value == "t2w" || value == "down")
    {
        *out_direction = ZoomDirection::TeleToWide;
        return true;
    }
    return false;
}

bool calibration_delta_for_ratio(const CalibrationProfile &profile, float ratio,
                                 bool use_conservative_bias, double strength, int32_t *out_delta)
{
    if (!out_delta)
    {
        return false;
    }
    *out_delta = 0;
    int32_t raw_delta = 0;
    const bool from_profile = profile_delta_raw(profile, ratio, &raw_delta);
    if (!from_profile && (!use_conservative_bias || !fixed_calibration_raw(ratio, &raw_delta)))
    {
        return false;
    }
    *out_delta = static_cast<int32_t>(std::lround(raw_delta * std::clamp(strength, 0.0, 1.0)));
    return true;
}

bool save_calibration_file(const char *path, const CalibrationProfile &profile)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        return false;
    }
    out << "# af_zoom_focus_calibration_v1\n";
    out << "# Generated by hal-auto-af-test. Keep this file per lens/sensor assembly.\n";
    out << std::fixed << std::setprecision(6);
    out << "distance_m=" << profile.distance_m << '\n';
    out << "zoom_ratio,table_focus,best_focus,delta_focus,confidence,accepted\n";
    for (const auto &point : profile.points)
    {
        out << point.zoom_ratio << ',' << point.table_focus << ',' << point.best_focus << ','
            << point.delta_focus << ',' << point.confidence << ',' << (point.accepted ? 1 : 0) << '\n';
    }
    return out.good();
}

bool load_calibration_file(const char *path, CalibrationProfile *out_profile)
{
    if (!path || !out_profile)
    {
        return false;
    }
    std::ifstream in(path);
    if (!in.is_open())
    {
        return false;
    }
    CalibrationProfile profile{};
    profile.path = path;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#')
        {
            continue;
        }
        static constexpr char distance_prefix[] = "distance_m=";
        if (line.compare(0, sizeof(distance_prefix) - 1u, distance_prefix) == 0)
        {
            profile.distance_m = static_cast<float>(std::atof(line.c_str() + sizeof(distance_prefix) - 1u));
            continue;
        }
        CalibrationPoint point{};
        int table_focus = 0;
        int best_focus = 0;
        int delta_focus = 0;
        int accepted = 0;
        double confidence = 0.0;
        if (std::sscanf(line.c_str(), "%f,%d,%d,%d,%lf,%d", &point.zoom_ratio, &table_focus, &best_focus,
                        &delta_focus, &confidence, &accepted) == 6)
        {
            point.table_focus = table_focus;
            point.best_focus = best_focus;
            point.delta_focus = delta_focus;
            point.confidence = confidence;
            point.accepted = accepted != 0;
            profile.points.push_back(point);
        }
    }
    std::sort(profile.points.begin(), profile.points.end(), [](const CalibrationPoint &a, const CalibrationPoint &b) {
        return a.zoom_ratio < b.zoom_ratio;
    });
    profile.loaded = std::any_of(profile.points.begin(), profile.points.end(),
                                 [](const CalibrationPoint &point) { return point.accepted; });
    if (!profile.loaded)
    {
        return false;
    }
    *out_profile = std::move(profile);
    return true;
}

bool aggregate_calibration_v2_point(float zoom_ratio, float distance_m, ZoomDirection direction,
                                    int32_t table_focus, const std::vector<CalibrationRepeat> &repeats,
                                    CalibrationV2Point *out_point)
{
    if (!out_point)
    {
        return false;
    }
    std::vector<double> deltas;
    std::vector<double> confidences;
    for (const auto &repeat : repeats)
    {
        if (!repeat.accepted)
        {
            continue;
        }
        deltas.push_back(static_cast<double>(repeat.best_focus - table_focus));
        confidences.push_back(repeat.confidence);
    }
    CalibrationV2Point point{};
    point.zoom_ratio = zoom_ratio;
    point.distance_m = distance_m;
    point.diopter = distance_m > 0.0f ? 1.0 / static_cast<double>(distance_m) : 0.0;
    point.zoom_direction = direction;
    point.table_focus = table_focus;
    if (deltas.empty())
    {
        *out_point = point;
        return false;
    }

    const double median_delta = median_value(deltas);
    std::vector<double> deviations;
    deviations.reserve(deltas.size());
    for (double delta : deltas)
    {
        deviations.push_back(std::fabs(delta - median_delta));
    }
    const double mad = median_value(deviations);
    const double limit = std::max(4.0, 3.0 * 1.4826 * mad);
    std::vector<double> filtered;
    std::vector<double> filtered_confidence;
    for (size_t i = 0; i < deltas.size(); ++i)
    {
        if (std::fabs(deltas[i] - median_delta) <= limit)
        {
            filtered.push_back(deltas[i]);
            filtered_confidence.push_back(confidences[i]);
        }
    }
    if (filtered.empty())
    {
        *out_point = point;
        return false;
    }
    const double final_delta = median_value(filtered);
    double variance = 0.0;
    for (double delta : filtered)
    {
        const double difference = delta - final_delta;
        variance += difference * difference;
    }
    if (filtered.size() > 1u)
    {
        variance /= static_cast<double>(filtered.size() - 1u);
    }
    point.delta_focus = static_cast<int32_t>(std::lround(final_delta));
    point.best_focus = table_focus + point.delta_focus;
    point.sigma_focus = std::sqrt(std::max(0.0, variance));
    point.sample_count = static_cast<int>(filtered.size());
    point.confidence = median_value(filtered_confidence);
    point.accepted = point.sample_count >= 3;
    *out_point = point;
    return point.accepted;
}

void upsert_calibration_v2_point(CalibrationProfileV2 *profile, const CalibrationV2Point &point)
{
    if (!profile)
    {
        return;
    }
    const auto matches = [&point](const CalibrationV2Point &existing) {
        return existing.zoom_direction == point.zoom_direction &&
               std::fabs(existing.zoom_ratio - point.zoom_ratio) < 0.001f &&
               std::fabs(existing.diopter - point.diopter) < 1e-6;
    };
    const auto found = std::find_if(profile->points.begin(), profile->points.end(), matches);
    if (found == profile->points.end())
    {
        profile->points.push_back(point);
    }
    else
    {
        *found = point;
    }
    profile->loaded = std::any_of(profile->points.begin(), profile->points.end(),
                                  [](const CalibrationV2Point &candidate) { return candidate.accepted; });
}

bool calibration_v2_lookup(const CalibrationProfileV2 &profile, float ratio, float distance_m,
                           ZoomDirection direction, double strength, int32_t *out_delta,
                           double *out_sigma)
{
    if (!out_delta || !profile.loaded)
    {
        return false;
    }
    const double query_diopter = distance_m > 0.0f ? 1.0 / static_cast<double>(distance_m) : 0.0;
    std::map<double, std::vector<const CalibrationV2Point *>> layers;
    for (const auto &point : profile.points)
    {
        if (point.accepted && point.zoom_direction == direction)
        {
            layers[point.diopter].push_back(&point);
        }
    }
    if (layers.empty())
    {
        return false;
    }
    std::vector<InterpolatedLayer> interpolated;
    for (const auto &layer : layers)
    {
        InterpolatedLayer value{};
        if (interpolate_zoom_layer(layer.second, ratio, &value))
        {
            value.diopter = layer.first;
            interpolated.push_back(value);
        }
    }
    if (interpolated.empty())
    {
        return false;
    }
    std::sort(interpolated.begin(), interpolated.end(), [](const InterpolatedLayer &a, const InterpolatedLayer &b) {
        return a.diopter < b.diopter;
    });
    const InterpolatedLayer *a = &interpolated.front();
    const InterpolatedLayer *b = &interpolated.front();
    if (query_diopter <= interpolated.front().diopter)
    {
        a = b = &interpolated.front();
    }
    else if (query_diopter >= interpolated.back().diopter)
    {
        a = b = &interpolated.back();
    }
    else
    {
        for (size_t i = 0; i + 1u < interpolated.size(); ++i)
        {
            if (query_diopter >= interpolated[i].diopter && query_diopter <= interpolated[i + 1u].diopter)
            {
                a = &interpolated[i];
                b = &interpolated[i + 1u];
                break;
            }
        }
    }
    const double denominator = b->diopter - a->diopter;
    const double t = std::fabs(denominator) > 1e-12
                         ? std::clamp((query_diopter - a->diopter) / denominator, 0.0, 1.0)
                         : 0.0;
    const double raw_delta = a->delta + (b->delta - a->delta) * t;
    const double interpolation_sigma = std::fabs(b->delta - a->delta) * t * (1.0 - t);
    const double sigma = a->sigma + (b->sigma - a->sigma) * t + interpolation_sigma;
    const double applied_strength = std::clamp(strength, 0.0, 1.0);
    *out_delta = static_cast<int32_t>(std::lround(raw_delta * applied_strength));
    if (out_sigma)
    {
        *out_sigma = std::max(0.0, sigma);
    }
    return true;
}

bool save_calibration_v2_file(const char *path, const CalibrationProfileV2 &profile)
{
    if (!path || path[0] == '\0')
    {
        return false;
    }
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out.is_open())
    {
        return false;
    }
    out << "# af_zoom_focus_calibration_v2\n";
    out << "# delta_focus=best_focus-table_focus; runtime adds delta_focus.\n";
    out << "zoom_ratio,distance_m,diopter,zoom_direction,table_focus,best_focus,delta_focus,"
           "sigma_focus,sample_count,confidence,accepted\n";
    out << std::fixed << std::setprecision(6);
    for (const auto &point : profile.points)
    {
        out << point.zoom_ratio << ',' << point.distance_m << ',' << point.diopter << ','
            << zoom_direction_name(point.zoom_direction) << ',' << point.table_focus << ','
            << point.best_focus << ',' << point.delta_focus << ',' << point.sigma_focus << ','
            << point.sample_count << ',' << point.confidence << ',' << (point.accepted ? 1 : 0) << '\n';
    }
    return out.good();
}

bool load_calibration_v2_file(const char *path, CalibrationProfileV2 *out_profile)
{
    if (!path || !out_profile)
    {
        return false;
    }
    std::ifstream in(path);
    if (!in.is_open())
    {
        return false;
    }
    CalibrationProfileV2 profile{};
    profile.path = path;
    std::string line;
    while (std::getline(in, line))
    {
        if (line.empty() || line[0] == '#' || line.rfind("zoom_ratio,", 0) == 0)
        {
            continue;
        }
        CalibrationV2Point point{};
        char direction[32]{};
        int accepted = 0;
        if (std::sscanf(line.c_str(), "%f,%f,%lf,%31[^,],%d,%d,%d,%lf,%d,%lf,%d",
                        &point.zoom_ratio, &point.distance_m, &point.diopter, direction,
                        &point.table_focus, &point.best_focus, &point.delta_focus,
                        &point.sigma_focus, &point.sample_count, &point.confidence, &accepted) != 11 ||
            !parse_zoom_direction(direction, &point.zoom_direction))
        {
            continue;
        }
        point.accepted = accepted != 0;
        profile.points.push_back(point);
    }
    profile.loaded = std::any_of(profile.points.begin(), profile.points.end(),
                                 [](const CalibrationV2Point &point) { return point.accepted; });
    if (!profile.loaded)
    {
        return false;
    }
    *out_profile = std::move(profile);
    return true;
}

} // namespace hal_auto_af
