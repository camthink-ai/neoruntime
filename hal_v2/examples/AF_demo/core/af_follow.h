#pragma once

#include <cstddef>
#include <vector>

namespace hal_auto_af
{

struct FollowConfig
{
    int follow_refine_span_steps = 96;
    int follow_fallback_span_steps = 256;
    int follow_edge_margin_steps = 16;
    double follow_default_distance_m = 3.0;
    double follow_confidence_min = 0.70;
    double follow_distance_alpha = 0.80;
    int follow_full_scan_on_low_confidence = 0;
    double follow_calibration_strength = 1.00;
    int follow_use_conservative_bias = 1;
    int follow_adaptive_span = 1;
    int follow_span_wide_steps = 48;
    int follow_span_mid_steps = 64;
    int follow_span_tele_steps = 80;
    int follow_recovery_span_steps = 96;
    int follow_fast_fine_span_steps = 16;
    int follow_path_zoom_step_wide = 240;
    int follow_path_zoom_step_mid = 160;
    int follow_path_zoom_step_tele = 160;
    int follow_path_focus_step_wide = 24;
    int follow_path_focus_step_mid = 50;
    int follow_path_focus_step_tele = 50;
    int follow_path_curve_error_enable = 1;
    int follow_path_curve_error_wide = 16;
    int follow_path_curve_error_mid = 8;
    int follow_path_curve_error_tele = 8;
    int follow_path_anchor_focus_tolerance = 16;
    int follow_path_zoom_tolerance = 8;
    int follow_path_endpoint_fast = 0;
    double follow_path_fast_confidence_min = 0.90;
    double follow_path_fast_reproducibility_min = 0.80;
    int follow_path_micro_refine = 0;
    int follow_path_micro_points = 3;
    int follow_path_micro_step = 8;
    int follow_path_micro_max_total_correction = 24;
    double follow_path_micro_min_gain_ratio = 0.05;
    double follow_path_micro_reproducibility_min = 0.80;
};

struct FollowState
{
    bool valid{false};
    float estimated_distance_m{0.0f};
    float last_zoom_ratio{1.0f};
    int last_best_focus{0};
    double last_metric{0.0};
};

enum class FollowAnchorStatus
{
    Valid,
    Missing,
    StaleZoom,
    StaleFocus,
};

const char *follow_anchor_status_name(FollowAnchorStatus status);
FollowAnchorStatus validate_follow_anchor(const FollowState &state, int current_zoom,
                                          int current_focus, int expected_zoom,
                                          const FollowConfig &config);

struct FollowPathSample
{
    float zoom_ratio{1.0f};
    int zoom_pos{0};
    int focus_pos{0};
};

struct FollowMicroRefineDecision
{
    bool accepted{false};
    int correction{0};
    double candidate_gain_ratio{0.0};
    double verified_gain_ratio{0.0};
    double reproducibility{0.0};
};

int refine_span_for_ratio(float ratio, int fine_span_steps, const FollowConfig &config);
float blend_finite_distance(float previous_distance_m, float measured_distance_m, double alpha);
int path_zoom_step_for_ratio(float ratio, const FollowConfig &config);
int path_focus_step_for_ratio(float ratio, const FollowConfig &config);
int path_curve_error_for_ratio(float ratio, const FollowConfig &config);
bool path_fast_endpoint_acceptable(bool valid, bool best_on_edge, double confidence,
                                   double reproducibility, const FollowConfig &config);
std::vector<std::size_t> select_follow_path_micro_checkpoints(std::size_t waypoint_count,
                                                              int requested_points);
FollowMicroRefineDecision evaluate_follow_path_micro_refine(
    int center_pos, double center_metric, int candidate_pos, double candidate_metric,
    double verified_metric, int cumulative_correction, const FollowConfig &config);
std::vector<FollowPathSample> select_follow_path_waypoints(const std::vector<FollowPathSample> &dense_path,
                                                           const FollowConfig &config);

} // namespace hal_auto_af
