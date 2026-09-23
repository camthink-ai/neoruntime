/**
 * @file inference_perf_sample.cpp
 * @brief Sample: HAL `query_system_performance_stats` (no inference session, no model/HEF).
 *
 * Usage:
 *   hal-inference-perf-sample [device_id]
 *     device_id: optional plain HailoRT id (e.g. device0). NOT JSON like {"device_id":"device0"} — that form
 *     is only for HalInferenceConfig.platform_config when opening an inference session.
 *     Omit the argument to use the default (first) device. No model/HEF.
 *
 * Polls up to three times and prints NPU/CPU/RAM/DSP fields (see HalInferencePerfStats).
 * On HAL_PLATFORM=stub (or no HailoRT), the first poll returns HAL_ERR_NOT_SUPPORTED and the program exits 0
 * after explaining that case; on a real target with HAL_OK, all three lines are printed.
 */

#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <cstdio>
#include <cstring>

static bool arg_looks_like_platform_config_json(const char *s)
{
    if (!s)
        return false;
    while (*s == ' ' || *s == '\t')
        ++s;
    return *s == '{';
}

static void print_perf(int index, const HalInferencePerfStats *p)
{
    std::printf(
        "sample %d: npu=%.2f%% cpu=%.2f%% ram_used=%lldKiB ram_total=%lldKiB dsp=%.2f%% soc_temp=%.1fC/%.1fC\n",
        index, static_cast<double>(p->npu_utilization), static_cast<double>(p->cpu_utilization),
        static_cast<long long>(p->ram_used_kib), static_cast<long long>(p->ram_total_kib),
        static_cast<double>(p->dsp_utilization), static_cast<double>(p->soc_temp_c),
        static_cast<double>(p->soc_temp_c1));
}

int main(int argc, char **argv)
{
    if (!HAL_INFERENCE_OPS.query_system_performance_stats)
    {
        std::fprintf(stderr, "HAL_INFERENCE_OPS.query_system_performance_stats is null\n");
        return 3;
    }

    const int bad_out =
        HAL_INFERENCE_OPS.query_system_performance_stats(nullptr, 100U, nullptr);
    if (bad_out != HAL_ERR_INVALID_ARG)
    {
        std::fprintf(stderr, "expected HAL_ERR_INVALID_ARG for null out, got %d (%s)\n", bad_out,
                     hal_error_to_string(static_cast<HalErrorCode>(bad_out)));
        return 2;
    }
    std::printf("null out -> %s (expected)\n", hal_error_to_string(static_cast<HalErrorCode>(bad_out)));
    std::fflush(stdout);

    const char *device_id = nullptr;
    if (argc >= 2 && argv[1][0] != '\0')
    {
        if (arg_looks_like_platform_config_json(argv[1]))
        {
            std::fprintf(stderr,
                         "Invalid device_id: looks like JSON platform_config.\n"
                         "Use a plain HailoRT device id, e.g.:\n"
                         "  %s\n"
                         "  %s device0\n"
                         "(JSON {\"device_id\":\"device0\"} is only for HAL inference create(), not this tool.)\n",
                         argv[0], argv[0]);
            return 2;
        }
        device_id = argv[1];
    }

    for (int i = 0; i < 3; ++i)
    {
        HalInferencePerfStats perf{};
        const int rc = HAL_INFERENCE_OPS.query_system_performance_stats(device_id, 100U, &perf);
        if (rc == HAL_ERR_NOT_SUPPORTED)
        {
            std::printf("query_system_performance_stats -> %s (e.g. stub build or no vendor HAL)\n",
                        hal_error_to_string(static_cast<HalErrorCode>(rc)));
            return 0;
        }
        if (rc != HAL_OK)
        {
            std::fprintf(stderr, "query_system_performance_stats: %d (%s)\n", rc,
                         hal_error_to_string(static_cast<HalErrorCode>(rc)));
            return 1;
        }
        print_perf(i, &perf);
    }

    std::printf("done.\n");
    return 0;
}
