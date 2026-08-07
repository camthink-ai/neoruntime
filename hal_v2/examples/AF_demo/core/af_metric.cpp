#include "af_metric.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace hal_auto_af
{
namespace
{

double median(std::vector<double> values)
{
    if (values.empty())
    {
        return 0.0;
    }
    const size_t mid = values.size() / 2u;
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid), values.end());
    const double upper = values[mid];
    if ((values.size() & 1u) != 0u)
    {
        return upper;
    }
    std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(mid - 1u), values.end());
    return 0.5 * (upper + values[mid - 1u]);
}

double percentile(std::vector<double> values, double q)
{
    if (values.empty())
    {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    const double p = std::clamp(q, 0.0, 1.0) * static_cast<double>(values.size() - 1u);
    const size_t lo = static_cast<size_t>(std::floor(p));
    const size_t hi = std::min(values.size() - 1u, lo + 1u);
    return values[lo] + (values[hi] - values[lo]) * (p - static_cast<double>(lo));
}

bool window_raw(const HalIspAfMeasurement &measurement, uint32_t index, const MetricConfig &config,
                double *out_raw, double *out_luma, double *out_brightness)
{
    if (!out_raw || !out_luma || !out_brightness || index >= measurement.window_count ||
        index >= HAL_ISP_AF_MAX_WINDOWS)
    {
        return false;
    }
    const uint32_t luma = measurement.luma[index];
    if (luma < static_cast<uint32_t>(config.metric_min_luma) ||
        (config.metric_max_luma > 0 && luma > static_cast<uint32_t>(config.metric_max_luma)))
    {
        return false;
    }
    double raw = static_cast<double>(measurement.sum[index]) / static_cast<double>(luma + 1u);
    if (config.metric_ratio_cap > 0.0 && raw > config.metric_ratio_cap)
    {
        raw = config.metric_ratio_cap;
    }

    double brightness = 1.0;
    if (config.metric_min_luma > 0)
    {
        const double t = (static_cast<double>(luma) - config.metric_min_luma) / config.metric_min_luma;
        brightness = std::min(brightness, 0.5 + 0.5 * std::clamp(t, 0.0, 1.0));
    }
    if (config.metric_max_luma > 0)
    {
        const double start = 0.8 * config.metric_max_luma;
        const double denom = std::max(1.0, config.metric_max_luma - start);
        brightness = std::min(brightness,
                              std::clamp((config.metric_max_luma - static_cast<double>(luma)) / denom, 0.0, 1.0));
    }
    *out_raw = raw;
    *out_luma = static_cast<double>(luma);
    *out_brightness = brightness;
    return true;
}

double combine_observation(const MetricObservation &observation, const MetricConfig &config)
{
    struct WindowMetric
    {
        double raw;
        double weight;
        uint32_t index;
    };
    std::vector<WindowMetric> values;
    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        if ((observation.valid_mask & (1u << i)) == 0u)
        {
            continue;
        }
        const double reliability = std::max(0.0, observation.brightness[i]) *
                                   std::max(0.0, observation.stability[i]) *
                                   std::max(0.0, observation.texture[i]);
        const double weight = static_cast<double>(std::max(0, config.metric_weights[i])) / 100.0 * reliability;
        values.push_back({observation.score[i], weight, i});
    }
    if (values.empty())
    {
        return 0.0;
    }
    if (config.metric_use_topk != 0)
    {
        std::sort(values.begin(), values.end(), [](const WindowMetric &a, const WindowMetric &b) {
            return (a.raw != b.raw) ? (a.raw > b.raw) : (a.index < b.index);
        });
        values.resize(static_cast<size_t>(std::clamp(config.metric_topk, 1, static_cast<int>(values.size()))));
    }

    std::vector<double> raw_values;
    raw_values.reserve(values.size());
    for (const auto &value : values)
    {
        raw_values.push_back(value.raw);
    }
    const double middle = median(raw_values);
    const double ratio = config.metric_outlier_ratio;
    const bool clamp_outliers = ratio > 1.0 && middle > 1e-12 && values.size() >= 2u;
    const double lo = clamp_outliers ? middle / ratio : -std::numeric_limits<double>::infinity();
    const double hi = clamp_outliers ? middle * ratio : std::numeric_limits<double>::infinity();

    double total = 0.0;
    double weight_sum = 0.0;
    double fallback = 0.0;
    for (const auto &value : values)
    {
        const double raw = std::clamp(value.raw, lo, hi);
        total += raw * value.weight;
        weight_sum += value.weight;
        fallback += raw;
    }
    return weight_sum > 1e-12 ? total / weight_sum : fallback / static_cast<double>(values.size());
}

} // namespace

MetricObservation observation_from_frames(const std::vector<HalIspAfMeasurement> &frames,
                                          const MetricConfig &config)
{
    MetricObservation observation{};
    observation.frame_count = static_cast<int>(frames.size());
    observation.texture.fill(1.0);
    observation.stability.fill(1.0);
    std::array<std::vector<double>, HAL_ISP_AF_MAX_WINDOWS> raw_values;
    std::array<std::vector<double>, HAL_ISP_AF_MAX_WINDOWS> luma_values;
    std::array<std::vector<double>, HAL_ISP_AF_MAX_WINDOWS> brightness_values;
    for (const auto &frame : frames)
    {
        if (frame.frame_id != 0u)
        {
            observation.frame_id = frame.frame_id;
        }
        if (frame.timestamp_ns != 0u)
        {
            observation.timestamp_ns = frame.timestamp_ns;
        }
        for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
        {
            double raw = 0.0;
            double luma = 0.0;
            double brightness = 0.0;
            if (window_raw(frame, i, config, &raw, &luma, &brightness))
            {
                raw_values[i].push_back(raw);
                luma_values[i].push_back(luma);
                brightness_values[i].push_back(brightness);
            }
        }
    }
    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        if (raw_values[i].empty())
        {
            continue;
        }
        const double raw_median = median(raw_values[i]);
        std::vector<double> absolute_deviation;
        absolute_deviation.reserve(raw_values[i].size());
        for (double value : raw_values[i])
        {
            absolute_deviation.push_back(std::fabs(value - raw_median));
        }
        const double relative_mad = median(absolute_deviation) / std::max(std::fabs(raw_median), 1e-9);
        double stability = 1.0;
        if (config.metric_temporal_rel_mad_limit > 1e-12)
        {
            stability = std::clamp(1.0 - relative_mad / config.metric_temporal_rel_mad_limit, 0.10, 1.0);
        }
        observation.raw[i] = raw_median;
        observation.score[i] = raw_median;
        const double luma_median = median(luma_values[i]);
        std::vector<double> luma_absolute_deviation;
        luma_absolute_deviation.reserve(luma_values[i].size());
        for (double value : luma_values[i])
        {
            luma_absolute_deviation.push_back(std::fabs(value - luma_median));
        }
        observation.luma[i] = luma_median;
        observation.luma_relative_mad[i] =
            median(luma_absolute_deviation) / std::max(std::fabs(luma_median), 1e-9);
        observation.brightness[i] = median(brightness_values[i]);
        observation.relative_mad[i] = relative_mad;
        observation.stability[i] = stability;
        observation.valid_mask |= 1u << i;
    }
    observation.metric = combine_observation(observation, config);
    double luma_sum = 0.0;
    double luma_relative_mad_sum = 0.0;
    double stability_sum = 0.0;
    double weight_sum = 0.0;
    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        if ((observation.valid_mask & (1u << i)) == 0u)
        {
            continue;
        }
        const double weight = static_cast<double>(std::max(0, config.metric_weights[i]));
        const double effective_weight = weight > 0.0 ? weight : 1.0;
        luma_sum += observation.luma[i] * effective_weight;
        luma_relative_mad_sum += observation.luma_relative_mad[i] * effective_weight;
        stability_sum += observation.stability[i] * effective_weight;
        weight_sum += effective_weight;
    }
    if (weight_sum > 0.0)
    {
        observation.mean_luma = luma_sum / weight_sum;
        observation.mean_luma_relative_mad = luma_relative_mad_sum / weight_sum;
        observation.temporal_stability = stability_sum / weight_sum;
    }
    return observation;
}

double metric_from_measurement(const HalIspAfMeasurement &measurement, const MetricConfig &config)
{
    return observation_from_frames(std::vector<HalIspAfMeasurement>{measurement}, config).metric;
}

double luma_stability_from_observation(const MetricObservation &observation,
                                       double relative_mad_limit)
{
    if (observation.frame_count < 2 || observation.valid_mask == 0u)
    {
        return 0.0;
    }
    const double limit = std::max(relative_mad_limit, 1e-9);
    return std::clamp(1.0 - observation.mean_luma_relative_mad / limit, 0.0, 1.0);
}

TextureModel build_texture_model(std::initializer_list<const std::vector<FocusSample> *> curves,
                                 const MetricConfig &config)
{
    TextureModel model{};
    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        std::vector<double> values;
        for (const auto *curve : curves)
        {
            if (!curve)
            {
                continue;
            }
            for (const auto &sample : *curve)
            {
                if ((sample.observation.valid_mask & (1u << i)) != 0u)
                {
                    values.push_back(sample.observation.raw[i]);
                }
            }
        }
        if (values.size() < 4u || config.metric_texture_response <= 1e-12)
        {
            continue;
        }
        const double p10 = percentile(values, 0.10);
        const double p50 = percentile(values, 0.50);
        const double p90 = percentile(values, 0.90);
        const double response = std::max(0.0, p90 - p10) / std::max(std::fabs(p50), 1e-9);
        const double scaled = std::clamp(response / config.metric_texture_response, 0.0, 1.0);
        model.reliability[i] = std::max(std::clamp(config.metric_texture_floor, 0.0, 1.0), scaled);
        model.p10[i] = p10;
        model.p90[i] = p90;
        model.normalize[i] = p90 > p10 + 1e-12;
    }
    return model;
}

void apply_texture_model(MetricObservation *observation, const TextureModel &model, const MetricConfig &config)
{
    if (!observation)
    {
        return;
    }
    observation->texture = model.reliability;
    for (uint32_t i = 0; i < HAL_ISP_AF_MAX_WINDOWS; ++i)
    {
        if ((observation->valid_mask & (1u << i)) == 0u)
        {
            continue;
        }
        observation->score[i] = observation->raw[i];
        if (model.normalize[i])
        {
            const double normalized = (observation->raw[i] - model.p10[i]) / (model.p90[i] - model.p10[i]);
            observation->score[i] = 0.25 + 0.75 * std::clamp(normalized, 0.0, 1.0);
        }
    }
    observation->metric = combine_observation(*observation, config);
}

void apply_texture_reliability(std::vector<FocusSample> *curve, const TextureModel &model,
                               const MetricConfig &config)
{
    if (!curve)
    {
        return;
    }
    for (auto &sample : *curve)
    {
        apply_texture_model(&sample.observation, model, config);
        sample.m = sample.observation.metric;
    }
}

} // namespace hal_auto_af
