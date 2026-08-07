#pragma once

#include "af_metric.h"

#include <vector>

namespace hal_auto_af
{

struct DirectionalProbeDecision
{
    int direction{0}; /* -1 = left, 0 = center, +1 = right */
    bool ambiguous{false};
    double relative_gain{0.0};
};

enum class PeakType
{
    ClosedPeak,
    PlateauPeak,
    OneSideWeak,
    OpenLeft,
    OpenRight,
    EdgePeak,
};

const char *peak_type_name(PeakType type);

struct PeakClosureConfig
{
    double noise_floor = 0.03;
    double plateau_ratio = 0.03;
    int focus_step = 0;
    int open_boundary_margin = 0;
};

struct PeakClosureResult
{
    PeakType type{PeakType::EdgePeak};
    double epsilon_ratio{0.0};
    double score{0.0};
    int left_samples{0};
    int right_samples{0};
    bool center_dominant{false};
    bool left_closed{false};
    bool right_closed{false};
    int suggested_direction{0}; /* -1 = confirm left, +1 = confirm right */
};

PeakClosureResult classify_peak(const std::vector<FocusSample> &curve, int peak_pos,
                                int scan_lo, int scan_hi, int mechanical_min, int mechanical_max,
                                const PeakClosureConfig &config);

struct ConfidenceV2Inputs
{
    double reproducibility{0.0};
    PeakType peak_type{PeakType::EdgePeak};
    double prominence_score{0.0};
    double temporal_stability{0.0};
    double texture_coverage{0.0};
    double luma_stability{0.0};
    bool scene_stable{true};
    bool verification_passed{true};
};

double peak_closure_score(PeakType type);
double confidence_v2(const ConfidenceV2Inputs &inputs);

DirectionalProbeDecision choose_directional_probe(double center_metric, double left_metric,
                                                   double right_metric, double min_gain_ratio);

bool fit_stop_extend_right(const std::vector<FocusSample> &samples, int step, int min_fit_samples,
                           double min_curvature, int lo, int hi, int min_spread);
bool fit_stop_extend_left(const std::vector<FocusSample> &samples, int step, int min_fit_samples,
                          double min_curvature, int lo, int hi, int min_spread);
double fitted_peak_x(const std::vector<FocusSample> &curve, int min_fit_samples, double min_curvature);

void merge_curves(const std::vector<FocusSample> &a, const std::vector<FocusSample> &b,
                  std::vector<FocusSample> *out_sorted);
void discrete_best(const std::vector<FocusSample> &curve, int *out_pos, double *out_metric);
double metric_at(const std::vector<FocusSample> &curve, int pos);
double peak_prominence(const std::vector<FocusSample> &curve, int peak_pos);

} // namespace hal_auto_af
