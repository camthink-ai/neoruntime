#pragma once

#include "media/hal_isp.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace hal_auto_af
{

struct MetricConfig
{
    int metric_min_luma = 4096;
    int metric_max_luma = 0;
    double metric_ratio_cap = 0.0;
    int metric_weights[HAL_ISP_AF_MAX_WINDOWS] = {25, 50, 25};
    int metric_use_topk = 0;
    int metric_topk = 3;
    double metric_temporal_rel_mad_limit = 0.25;
    double metric_outlier_ratio = 3.0;
    double metric_texture_response = 0.12;
    double metric_texture_floor = 0.15;
};

struct MetricObservation
{
    double metric{0.0};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> raw{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> score{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> luma{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> stability{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> brightness{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> texture{{1.0, 1.0, 1.0}};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> relative_mad{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> luma_relative_mad{};
    uint32_t valid_mask{0};
    int frame_count{0};
    uint64_t frame_id{0};
    uint64_t timestamp_ns{0};
    double mean_luma{0.0};
    double mean_luma_relative_mad{0.0};
    double temporal_stability{0.0};
};

struct FocusSample
{
    int pos{};
    double m{};
    MetricObservation observation{};
};

struct TextureModel
{
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> reliability{{1.0, 1.0, 1.0}};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> p10{};
    std::array<double, HAL_ISP_AF_MAX_WINDOWS> p90{};
    std::array<bool, HAL_ISP_AF_MAX_WINDOWS> normalize{};
};

MetricObservation observation_from_frames(const std::vector<HalIspAfMeasurement> &frames,
                                          const MetricConfig &config);
double metric_from_measurement(const HalIspAfMeasurement &measurement, const MetricConfig &config);
double luma_stability_from_observation(const MetricObservation &observation,
                                       double relative_mad_limit);

TextureModel build_texture_model(std::initializer_list<const std::vector<FocusSample> *> curves,
                                 const MetricConfig &config);
void apply_texture_model(MetricObservation *observation, const TextureModel &model,
                         const MetricConfig &config);
void apply_texture_reliability(std::vector<FocusSample> *curve, const TextureModel &model,
                               const MetricConfig &config);

} // namespace hal_auto_af
