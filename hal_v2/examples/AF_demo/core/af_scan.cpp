#include "af_scan.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <utility>

namespace hal_auto_af
{
namespace
{

void merge_samples_for_fit(const std::vector<FocusSample> &input, std::vector<double> *xs,
                           std::vector<double> *ys)
{
    std::map<int, std::pair<double, int>> accumulated;
    for (const auto &sample : input)
    {
        auto &entry = accumulated[sample.pos];
        entry.first += sample.m;
        ++entry.second;
    }
    xs->clear();
    ys->clear();
    for (const auto &entry : accumulated)
    {
        xs->push_back(static_cast<double>(entry.first));
        ys->push_back(entry.second.first / static_cast<double>(entry.second.second));
    }
}

bool solve_linear_gauss3(double matrix[3][4])
{
    for (int column = 0; column < 3; ++column)
    {
        int pivot = column;
        for (int row = column; row < 3; ++row)
        {
            if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column]))
            {
                pivot = row;
            }
        }
        if (std::fabs(matrix[pivot][column]) < 1e-18)
        {
            return false;
        }
        if (pivot != column)
        {
            for (int c = column; c < 4; ++c)
            {
                std::swap(matrix[pivot][c], matrix[column][c]);
            }
        }
        const double divisor = matrix[column][column];
        for (int c = column; c < 4; ++c)
        {
            matrix[column][c] /= divisor;
        }
        for (int row = 0; row < 3; ++row)
        {
            if (row == column)
            {
                continue;
            }
            const double factor = matrix[row][column];
            for (int c = column; c < 4; ++c)
            {
                matrix[row][c] -= factor * matrix[column][c];
            }
        }
    }
    return true;
}

bool quadratic_fit(const std::vector<double> &x, const std::vector<double> &y, double min_curvature,
                   double *out_vertex_x)
{
    const size_t count = x.size();
    if (count < 3u || y.size() != count || !out_vertex_x)
    {
        return false;
    }
    double p4 = 0.0;
    double p3 = 0.0;
    double p2 = 0.0;
    double p1 = 0.0;
    const double p0 = static_cast<double>(count);
    double q2 = 0.0;
    double q1 = 0.0;
    double q0 = 0.0;
    for (size_t i = 0; i < count; ++i)
    {
        const double x2 = x[i] * x[i];
        const double x3 = x2 * x[i];
        p1 += x[i];
        p2 += x2;
        p3 += x3;
        p4 += x2 * x2;
        q0 += y[i];
        q1 += y[i] * x[i];
        q2 += y[i] * x2;
    }
    double matrix[3][4] = {
        {p4, p3, p2, q2},
        {p3, p2, p1, q1},
        {p2, p1, p0, q0},
    };
    if (!solve_linear_gauss3(matrix))
    {
        return false;
    }
    const double a = matrix[0][3];
    const double b = matrix[1][3];
    if (a >= -min_curvature)
    {
        return false;
    }
    const double vertex_x = -b / (2.0 * a);
    if (!std::isfinite(vertex_x))
    {
        return false;
    }
    *out_vertex_x = vertex_x;
    return true;
}

bool spread_is_sufficient(const std::vector<double> &xs, int lo, int hi, int min_spread)
{
    if (min_spread <= 0 || hi <= lo)
    {
        return true;
    }
    const int spread = static_cast<int>(std::lround(xs.back() - xs.front()));
    return spread >= std::min(min_spread, hi - lo);
}

double sample_relative_mad(const FocusSample &sample)
{
    double total = 0.0;
    int count = 0;
    for (size_t i = 0; i < sample.observation.relative_mad.size(); ++i)
    {
        if ((sample.observation.valid_mask & (1u << i)) == 0u)
        {
            continue;
        }
        total += std::max(0.0, sample.observation.relative_mad[i]);
        ++count;
    }
    return count > 0 ? total / static_cast<double>(count) : 0.0;
}

} // namespace

const char *peak_type_name(PeakType type)
{
    switch (type)
    {
        case PeakType::ClosedPeak: return "closed_peak";
        case PeakType::PlateauPeak: return "plateau_peak";
        case PeakType::OneSideWeak: return "one_side_weak";
        case PeakType::OpenLeft: return "open_left";
        case PeakType::OpenRight: return "open_right";
        case PeakType::EdgePeak: return "edge_peak";
        default: return "unknown";
    }
}

double peak_closure_score(PeakType type)
{
    switch (type)
    {
        case PeakType::ClosedPeak: return 1.00;
        case PeakType::PlateauPeak: return 0.85;
        case PeakType::OneSideWeak: return 0.55;
        case PeakType::OpenLeft:
        case PeakType::OpenRight: return 0.30;
        case PeakType::EdgePeak: return 0.00;
        default: return 0.00;
    }
}

PeakClosureResult classify_peak(const std::vector<FocusSample> &curve, int peak_pos,
                                int scan_lo, int scan_hi, int mechanical_min, int mechanical_max,
                                const PeakClosureConfig &config)
{
    PeakClosureResult result{};
    std::map<int, FocusSample> unique;
    for (const auto &sample : curve)
    {
        const auto found = unique.find(sample.pos);
        if (found == unique.end() || sample.observation.frame_count >= found->second.observation.frame_count)
        {
            unique[sample.pos] = sample;
        }
    }
    std::vector<FocusSample> sorted;
    sorted.reserve(unique.size());
    for (const auto &entry : unique)
    {
        sorted.push_back(entry.second);
    }
    const auto peak = std::find_if(sorted.begin(), sorted.end(), [peak_pos](const FocusSample &sample) {
        return sample.pos == peak_pos;
    });
    if (peak == sorted.end())
    {
        return result;
    }
    const size_t index = static_cast<size_t>(std::distance(sorted.begin(), peak));
    result.left_samples = static_cast<int>(index);
    result.right_samples = static_cast<int>(sorted.size() - index - 1u);

    if (config.focus_step > 0)
    {
        const int step = std::max(1, config.focus_step);
        const auto sample_at = [&unique](int pos) -> const FocusSample * {
            const auto found = unique.find(pos);
            return found == unique.end() ? nullptr : &found->second;
        };
        const FocusSample *const l1 = sample_at(peak_pos - step);
        const FocusSample *const l2 = sample_at(peak_pos - 2 * step);
        const FocusSample *const r1 = sample_at(peak_pos + step);
        const FocusSample *const r2 = sample_at(peak_pos + 2 * step);
        result.left_samples = (l1 ? 1 : 0) + (l2 ? 1 : 0);
        result.right_samples = (r1 ? 1 : 0) + (r2 ? 1 : 0);

        double local_noise = sample_relative_mad(*peak);
        int local_count = 1;
        for (const FocusSample *sample : {l2, l1, r1, r2})
        {
            if (sample)
            {
                local_noise += sample_relative_mad(*sample);
                ++local_count;
            }
        }
        local_noise /= static_cast<double>(local_count);
        result.epsilon_ratio = std::clamp(std::max(config.noise_floor, 3.0 * local_noise), 0.0, 0.50);

        const bool mechanical_left = peak_pos <= mechanical_min;
        const bool mechanical_right = peak_pos >= mechanical_max;
        if (mechanical_left || mechanical_right)
        {
            result.type = PeakType::EdgePeak;
            result.suggested_direction = mechanical_left ? -1 : 1;
            result.score = peak_closure_score(result.type);
            return result;
        }

        const int boundary_margin = std::max(0, config.open_boundary_margin);
        const bool near_left_boundary = peak_pos - scan_lo <= boundary_margin;
        const bool near_right_boundary = scan_hi - peak_pos <= boundary_margin;
        if (!l1 || !l2 || !r1 || !r2)
        {
            if ((!l1 || !l2) && near_left_boundary)
            {
                result.type = scan_lo <= mechanical_min ? PeakType::EdgePeak : PeakType::OpenLeft;
                result.suggested_direction = -1;
            }
            else if ((!r1 || !r2) && near_right_boundary)
            {
                result.type = scan_hi >= mechanical_max ? PeakType::EdgePeak : PeakType::OpenRight;
                result.suggested_direction = 1;
            }
            else
            {
                result.type = PeakType::OneSideWeak;
                if (!l1 || !l2)
                {
                    result.suggested_direction = -1;
                }
                else if (!r1 || !r2)
                {
                    result.suggested_direction = 1;
                }
            }
            result.score = peak_closure_score(result.type);
            return result;
        }

        const double p = peak->m;
        const double epsilon = result.epsilon_ratio * std::max(std::fabs(p), 1e-9);
        const double plateau_epsilon = std::max(config.plateau_ratio, result.epsilon_ratio) *
                                       std::max(std::fabs(p), 1e-9);
        result.center_dominant = p + epsilon >= l1->m && p + epsilon >= r1->m;
        result.left_closed = l1->m > l2->m + epsilon;
        result.right_closed = r1->m > r2->m + epsilon;
        const bool plateau = std::fabs(p - l1->m) <= plateau_epsilon ||
                             std::fabs(p - r1->m) <= plateau_epsilon;

        if (result.center_dominant && result.left_closed && result.right_closed)
        {
            result.type = plateau ? PeakType::PlateauPeak : PeakType::ClosedPeak;
        }
        else
        {
            if (!result.center_dominant)
            {
                result.suggested_direction = l1->m > r1->m ? -1 : 1;
            }
            else if (!result.left_closed && result.right_closed)
            {
                result.suggested_direction = -1;
            }
            else if (result.left_closed && !result.right_closed)
            {
                result.suggested_direction = 1;
            }

            const bool confirmed_open_left = result.suggested_direction < 0 && near_left_boundary &&
                                             l2->m + epsilon >= l1->m;
            const bool confirmed_open_right = result.suggested_direction > 0 && near_right_boundary &&
                                              r2->m + epsilon >= r1->m;
            if (confirmed_open_left)
            {
                result.type = PeakType::OpenLeft;
            }
            else if (confirmed_open_right)
            {
                result.type = PeakType::OpenRight;
            }
            else
            {
                /* Interior non-monotonic samples are weak evidence, not a proven open peak. */
                result.type = PeakType::OneSideWeak;
            }
        }
        result.score = peak_closure_score(result.type);
        return result;
    }

    double local_noise = sample_relative_mad(*peak);
    int local_count = 1;
    for (int offset : {-2, -1, 1, 2})
    {
        const int candidate = static_cast<int>(index) + offset;
        if (candidate >= 0 && candidate < static_cast<int>(sorted.size()))
        {
            local_noise += sample_relative_mad(sorted[static_cast<size_t>(candidate)]);
            ++local_count;
        }
    }
    local_noise /= static_cast<double>(local_count);
    result.epsilon_ratio = std::clamp(std::max(config.noise_floor, 3.0 * local_noise), 0.0, 0.50);

    const bool mechanical_edge = peak_pos <= mechanical_min || peak_pos >= mechanical_max;
    const bool scan_edge = peak_pos <= scan_lo || peak_pos >= scan_hi;
    if (mechanical_edge)
    {
        result.type = PeakType::EdgePeak;
        result.score = peak_closure_score(result.type);
        return result;
    }
    if (result.left_samples == 0)
    {
        result.type = scan_edge && scan_lo <= mechanical_min ? PeakType::EdgePeak : PeakType::OpenLeft;
        result.score = peak_closure_score(result.type);
        return result;
    }
    if (result.right_samples == 0)
    {
        result.type = scan_edge && scan_hi >= mechanical_max ? PeakType::EdgePeak : PeakType::OpenRight;
        result.score = peak_closure_score(result.type);
        return result;
    }
    if (result.left_samples < 2 || result.right_samples < 2)
    {
        result.type = PeakType::OneSideWeak;
        result.score = peak_closure_score(result.type);
        return result;
    }

    const double p = peak->m;
    const double l1 = sorted[index - 1u].m;
    const double l2 = sorted[index - 2u].m;
    const double r1 = sorted[index + 1u].m;
    const double r2 = sorted[index + 2u].m;
    const double epsilon = result.epsilon_ratio * std::max(std::fabs(p), 1e-9);
    const double plateau_epsilon = std::max(config.plateau_ratio, result.epsilon_ratio) *
                                   std::max(std::fabs(p), 1e-9);
    const bool peak_not_below_flanks = p + epsilon >= l1 && p + epsilon >= r1;
    const bool left_descends = l1 > l2 + epsilon;
    const bool right_descends = r1 > r2 + epsilon;
    const bool plateau = std::fabs(p - l1) <= plateau_epsilon || std::fabs(p - r1) <= plateau_epsilon;

    if (peak_not_below_flanks && left_descends && right_descends)
    {
        result.type = plateau ? PeakType::PlateauPeak : PeakType::ClosedPeak;
    }
    else if (!left_descends && right_descends)
    {
        result.type = PeakType::OpenLeft;
    }
    else if (left_descends && !right_descends)
    {
        result.type = PeakType::OpenRight;
    }
    else
    {
        result.type = scan_edge ? PeakType::EdgePeak : PeakType::OneSideWeak;
    }
    result.score = peak_closure_score(result.type);
    return result;
}

double confidence_v2(const ConfidenceV2Inputs &inputs)
{
    if (!inputs.scene_stable)
    {
        return 0.0;
    }
    const double reproducibility = std::clamp(inputs.reproducibility, 0.0, 1.0);
    const double value = 0.50 * reproducibility +
                         0.15 * std::clamp(inputs.prominence_score, 0.0, 1.0) +
                         0.15 * std::clamp(inputs.temporal_stability, 0.0, 1.0) +
                         0.10 * std::clamp(inputs.texture_coverage, 0.0, 1.0) +
                         0.10 * std::clamp(inputs.luma_stability, 0.0, 1.0);
    const bool open_or_edge = inputs.peak_type == PeakType::OpenLeft ||
                              inputs.peak_type == PeakType::OpenRight ||
                              inputs.peak_type == PeakType::EdgePeak;
    double confidence = std::clamp(value, 0.0, 1.0);
    if (open_or_edge || !inputs.verification_passed)
    {
        confidence = std::min(confidence, 0.60);
    }
    return confidence;
}

DirectionalProbeDecision choose_directional_probe(double center_metric, double left_metric,
                                                   double right_metric, double min_gain_ratio)
{
    DirectionalProbeDecision decision{};
    if (center_metric >= left_metric && center_metric >= right_metric)
    {
        return decision;
    }
    if (right_metric >= left_metric)
    {
        decision.direction = 1;
        const double runner_up = std::max(center_metric, left_metric);
        decision.relative_gain = (right_metric - runner_up) / std::max(std::fabs(runner_up), 1e-9);
    }
    else
    {
        decision.direction = -1;
        const double runner_up = std::max(center_metric, right_metric);
        decision.relative_gain = (left_metric - runner_up) / std::max(std::fabs(runner_up), 1e-9);
    }
    decision.ambiguous = decision.relative_gain < std::max(0.0, min_gain_ratio);
    return decision;
}

bool fit_stop_extend_right(const std::vector<FocusSample> &samples, int step, int min_fit_samples,
                           double min_curvature, int lo, int hi, int min_spread)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_samples_for_fit(samples, &xs, &ys);
    if (xs.size() < static_cast<size_t>(std::max(3, min_fit_samples)) ||
        !spread_is_sufficient(xs, lo, hi, min_spread))
    {
        return false;
    }
    double vertex_x = 0.0;
    return quadratic_fit(xs, ys, min_curvature, &vertex_x) &&
           xs.back() + 1e-6 >= vertex_x + 0.5 * static_cast<double>(std::max(1, step));
}

bool fit_stop_extend_left(const std::vector<FocusSample> &samples, int step, int min_fit_samples,
                          double min_curvature, int lo, int hi, int min_spread)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_samples_for_fit(samples, &xs, &ys);
    if (xs.size() < static_cast<size_t>(std::max(3, min_fit_samples)) ||
        !spread_is_sufficient(xs, lo, hi, min_spread))
    {
        return false;
    }
    double vertex_x = 0.0;
    return quadratic_fit(xs, ys, min_curvature, &vertex_x) &&
           xs.front() - 1e-6 <= vertex_x - 0.5 * static_cast<double>(std::max(1, step));
}

double fitted_peak_x(const std::vector<FocusSample> &curve, int min_fit_samples, double min_curvature)
{
    std::vector<double> xs;
    std::vector<double> ys;
    merge_samples_for_fit(curve, &xs, &ys);
    double vertex_x = 0.0;
    if (xs.size() >= static_cast<size_t>(std::max(3, min_fit_samples)) &&
        quadratic_fit(xs, ys, min_curvature, &vertex_x))
    {
        return vertex_x;
    }
    if (curve.empty())
    {
        return 0.0;
    }
    int best_pos = curve.front().pos;
    double best_metric = curve.front().m;
    for (const auto &sample : curve)
    {
        if (sample.m > best_metric)
        {
            best_metric = sample.m;
            best_pos = sample.pos;
        }
    }
    return static_cast<double>(best_pos);
}

void merge_curves(const std::vector<FocusSample> &a, const std::vector<FocusSample> &b,
                  std::vector<FocusSample> *out_sorted)
{
    if (!out_sorted)
    {
        return;
    }
    std::map<int, FocusSample> samples;
    for (const auto &sample : a)
    {
        samples[sample.pos] = sample;
    }
    for (const auto &sample : b)
    {
        samples[sample.pos] = sample;
    }
    out_sorted->clear();
    out_sorted->reserve(samples.size());
    for (const auto &entry : samples)
    {
        out_sorted->push_back(entry.second);
    }
}

void discrete_best(const std::vector<FocusSample> &curve, int *out_pos, double *out_metric)
{
    if (!out_pos || !out_metric || curve.empty())
    {
        return;
    }
    int best_pos = curve.front().pos;
    double best_metric = curve.front().m;
    for (const auto &sample : curve)
    {
        if (sample.m > best_metric)
        {
            best_pos = sample.pos;
            best_metric = sample.m;
        }
    }
    *out_pos = best_pos;
    *out_metric = best_metric;
}

double metric_at(const std::vector<FocusSample> &curve, int pos)
{
    for (const auto &sample : curve)
    {
        if (sample.pos == pos)
        {
            return sample.m;
        }
    }
    return 0.0;
}

double peak_prominence(const std::vector<FocusSample> &curve, int peak_pos)
{
    if (curve.size() < 3u)
    {
        return 0.0;
    }
    size_t index = curve.size();
    for (size_t i = 0; i < curve.size(); ++i)
    {
        if (curve[i].pos == peak_pos)
        {
            index = i;
            break;
        }
    }
    if (index == 0u || index + 1u >= curve.size())
    {
        return 0.0;
    }
    const double flank = 0.5 * (curve[index - 1u].m + curve[index + 1u].m);
    return std::max(0.0, (curve[index].m - flank) / std::max(std::fabs(flank), 1e-9));
}

} // namespace hal_auto_af
