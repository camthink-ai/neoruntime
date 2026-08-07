#include "af_follow.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace hal_auto_af
{

const char *follow_anchor_status_name(FollowAnchorStatus status)
{
    switch (status)
    {
        case FollowAnchorStatus::Valid: return "valid";
        case FollowAnchorStatus::Missing: return "missing";
        case FollowAnchorStatus::StaleZoom: return "stale_zoom";
        case FollowAnchorStatus::StaleFocus: return "stale_focus";
        default: return "unknown";
    }
}

FollowAnchorStatus validate_follow_anchor(const FollowState &state, int current_zoom,
                                          int current_focus, int expected_zoom,
                                          const FollowConfig &config)
{
    if (!state.valid)
    {
        return FollowAnchorStatus::Missing;
    }
    if (std::abs(current_zoom - expected_zoom) > std::max(0, config.follow_path_zoom_tolerance))
    {
        return FollowAnchorStatus::StaleZoom;
    }
    if (std::abs(current_focus - state.last_best_focus) >
        std::max(0, config.follow_path_anchor_focus_tolerance))
    {
        return FollowAnchorStatus::StaleFocus;
    }
    return FollowAnchorStatus::Valid;
}

int refine_span_for_ratio(float ratio, int fine_span_steps, const FollowConfig &config)
{
    if (config.follow_adaptive_span == 0)
    {
        return std::max(fine_span_steps, config.follow_refine_span_steps);
    }
    int span = config.follow_span_tele_steps;
    if (ratio <= 1.50f)
    {
        span = config.follow_span_wide_steps;
    }
    else if (ratio <= 2.25f)
    {
        span = config.follow_span_mid_steps;
    }
    return std::max(fine_span_steps, std::max(0, span));
}

float blend_finite_distance(float previous_distance_m, float measured_distance_m, double alpha)
{
    if (previous_distance_m <= 0.0f || measured_distance_m <= 0.0f)
    {
        return measured_distance_m;
    }
    const float weight = static_cast<float>(std::clamp(alpha, 0.0, 1.0));
    return (1.0f - weight) * previous_distance_m + weight * measured_distance_m;
}

int path_zoom_step_for_ratio(float ratio, const FollowConfig &config)
{
    if (ratio <= 1.50f)
    {
        return std::max(1, config.follow_path_zoom_step_wide);
    }
    if (ratio <= 2.25f)
    {
        return std::max(1, config.follow_path_zoom_step_mid);
    }
    return std::max(1, config.follow_path_zoom_step_tele);
}

int path_focus_step_for_ratio(float ratio, const FollowConfig &config)
{
    if (ratio <= 1.50f)
    {
        return std::max(1, config.follow_path_focus_step_wide);
    }
    if (ratio <= 2.25f)
    {
        return std::max(1, config.follow_path_focus_step_mid);
    }
    return std::max(1, config.follow_path_focus_step_tele);
}

int path_curve_error_for_ratio(float ratio, const FollowConfig &config)
{
    if (ratio <= 1.50f)
    {
        return std::max(0, config.follow_path_curve_error_wide);
    }
    if (ratio <= 2.25f)
    {
        return std::max(0, config.follow_path_curve_error_mid);
    }
    return std::max(0, config.follow_path_curve_error_tele);
}

static bool path_segment_follows_curve(const std::vector<FollowPathSample> &dense_path,
                                       std::size_t start, std::size_t end,
                                       const FollowConfig &config)
{
    if (config.follow_path_curve_error_enable == 0 || end <= start + 1u)
    {
        return true;
    }

    const FollowPathSample &a = dense_path[start];
    const FollowPathSample &b = dense_path[end];
    const double zoom_delta = static_cast<double>(b.zoom_pos - a.zoom_pos);
    const double focus_delta = static_cast<double>(b.focus_pos - a.focus_pos);
    const double index_span = static_cast<double>(end - start);

    for (std::size_t i = start + 1u; i < end; ++i)
    {
        double t = static_cast<double>(i - start) / index_span;
        if (std::abs(zoom_delta) > 1e-9)
        {
            t = static_cast<double>(dense_path[i].zoom_pos - a.zoom_pos) / zoom_delta;
        }
        t = std::clamp(t, 0.0, 1.0);
        const double linear_focus = static_cast<double>(a.focus_pos) + t * focus_delta;
        const double error = std::abs(
            static_cast<double>(dense_path[i].focus_pos) - linear_focus);
        if (error > static_cast<double>(
                path_curve_error_for_ratio(dense_path[i].zoom_ratio, config)))
        {
            return false;
        }
    }
    return true;
}

bool path_fast_endpoint_acceptable(bool valid, bool best_on_edge, double confidence,
                                   double reproducibility, const FollowConfig &config)
{
    const double confidence_min =
        std::max(std::clamp(config.follow_confidence_min, 0.0, 1.0),
                 std::clamp(config.follow_path_fast_confidence_min, 0.0, 1.0));
    const double reproducibility_min =
        std::clamp(config.follow_path_fast_reproducibility_min, 0.0, 1.0);
    return valid && !best_on_edge && confidence >= confidence_min &&
           reproducibility >= reproducibility_min;
}

std::vector<std::size_t> select_follow_path_micro_checkpoints(std::size_t waypoint_count,
                                                              int requested_points)
{
    std::vector<std::size_t> checkpoints;
    if (waypoint_count < 3u || requested_points <= 0)
    {
        return checkpoints;
    }

    static constexpr std::array<double, 1> one = {0.65};
    static constexpr std::array<double, 2> two = {0.40, 0.75};
    static constexpr std::array<double, 3> three = {0.35, 0.65, 0.85};
    const int count = std::clamp(requested_points, 1, 3);
    const double *fractions = count == 1 ? one.data() : (count == 2 ? two.data() : three.data());
    const std::size_t last = waypoint_count - 1u;
    checkpoints.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const auto rounded = static_cast<std::size_t>(std::llround(fractions[i] * static_cast<double>(last)));
        const std::size_t index = std::clamp<std::size_t>(rounded, 1u, last - 1u);
        if (checkpoints.empty() || checkpoints.back() != index)
        {
            checkpoints.push_back(index);
        }
    }
    return checkpoints;
}

FollowMicroRefineDecision evaluate_follow_path_micro_refine(
    int center_pos, double center_metric, int candidate_pos, double candidate_metric,
    double verified_metric, int cumulative_correction, const FollowConfig &config)
{
    FollowMicroRefineDecision decision{};
    if (center_metric <= 1e-12 || candidate_metric <= 1e-12 || verified_metric < 0.0)
    {
        return decision;
    }

    decision.candidate_gain_ratio = candidate_metric / center_metric - 1.0;
    decision.verified_gain_ratio = verified_metric / center_metric - 1.0;
    decision.reproducibility = std::clamp(verified_metric / candidate_metric, 0.0, 1.5);
    const int requested_correction = candidate_pos - center_pos;
    const int correction_limit = std::max(0, config.follow_path_micro_max_total_correction);
    const int correction_lo = -correction_limit - cumulative_correction;
    const int correction_hi = correction_limit - cumulative_correction;
    decision.correction = std::clamp(requested_correction, correction_lo, correction_hi);

    const double gain_min = std::max(0.0, config.follow_path_micro_min_gain_ratio);
    const double reproducibility_min =
        std::clamp(config.follow_path_micro_reproducibility_min, 0.0, 1.0);
    decision.accepted = requested_correction != 0 && decision.correction != 0 &&
                        decision.candidate_gain_ratio >= gain_min &&
                        decision.verified_gain_ratio >= gain_min &&
                        decision.reproducibility >= reproducibility_min;
    if (!decision.accepted)
    {
        decision.correction = 0;
    }
    return decision;
}

std::vector<FollowPathSample> select_follow_path_waypoints(const std::vector<FollowPathSample> &dense_path,
                                                           const FollowConfig &config)
{
    if (dense_path.size() <= 2u)
    {
        return dense_path;
    }

    std::vector<FollowPathSample> waypoints;
    waypoints.reserve(dense_path.size());
    waypoints.push_back(dense_path.front());

    size_t segment_start = 0;
    size_t candidate = 1;
    while (candidate < dense_path.size())
    {
        const FollowPathSample &a = dense_path[segment_start];
        const FollowPathSample &b = dense_path[candidate];
        const float limiting_ratio = std::max(a.zoom_ratio, b.zoom_ratio);
        const int zoom_limit = path_zoom_step_for_ratio(limiting_ratio, config);
        const int focus_limit = path_focus_step_for_ratio(limiting_ratio, config);
        const bool fits = std::abs(b.zoom_pos - a.zoom_pos) <= zoom_limit &&
                          std::abs(b.focus_pos - a.focus_pos) <= focus_limit &&
                          path_segment_follows_curve(
                              dense_path, segment_start, candidate, config);
        const bool is_last = candidate + 1u == dense_path.size();

        if (fits && !is_last)
        {
            ++candidate;
            continue;
        }

        size_t chosen = candidate;
        if (!fits && candidate > segment_start + 1u)
        {
            chosen = candidate - 1u;
        }
        if (waypoints.back().zoom_pos != dense_path[chosen].zoom_pos ||
            waypoints.back().focus_pos != dense_path[chosen].focus_pos)
        {
            waypoints.push_back(dense_path[chosen]);
        }
        segment_start = chosen;
        if (chosen == candidate)
        {
            ++candidate;
        }
    }

    const FollowPathSample &last = dense_path.back();
    if (waypoints.back().zoom_pos != last.zoom_pos || waypoints.back().focus_pos != last.focus_pos)
    {
        waypoints.push_back(last);
    }
    return waypoints;
}

} // namespace hal_auto_af
