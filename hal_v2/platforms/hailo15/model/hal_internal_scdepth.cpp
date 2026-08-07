/**
 * @file hal_internal_scdepth.cpp
 */

#include "hal_internal_scdepth.hpp"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include "common/hal_common.h"
#include "common/hal_log.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include <hailo_postprocess_tools/objects/hailo_tensors.hpp>

namespace hal_v2::internal_scdepth
{
namespace
{

static constexpr const char *kDefaultTensorName = "scdepthv3/conv31";

static std::string json_string_after_key(const std::string &j, const char *key)
{
    if (!key)
        return {};
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p == std::string::npos)
        return {};
    p = j.find(':', p);
    if (p == std::string::npos)
        return {};
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        p++;
    if (p >= j.size() || j[p] != '"')
        return {};
    p++;
    const size_t q = j.find('"', p);
    if (q == std::string::npos)
        return {};
    return j.substr(p, q - p);
}

static bool json_bool_after_key(const std::string &j, const char *key, bool *out)
{
    if (!key || !out)
        return false;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p == std::string::npos)
        return false;
    p = j.find(':', p);
    if (p == std::string::npos)
        return false;
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        p++;
    if (p + 4 <= j.size() && j.compare(p, 4, "true") == 0)
    {
        *out = true;
        return true;
    }
    if (p + 5 <= j.size() && j.compare(p, 5, "false") == 0)
    {
        *out = false;
        return true;
    }
    return false;
}

static size_t elem_index(uint32_t row, uint32_t col, uint32_t ch, uint32_t W, uint32_t F)
{
    return (size_t)W * (size_t)F * (size_t)row + (size_t)F * (size_t)col + (size_t)ch;
}

static float read_logit(HailoTensorPtr t, uint32_t row, uint32_t col, bool as_float32)
{
    if (!t || !t->data())
        return 0.f;
    const uint32_t W = t->width();
    const uint32_t F = t->features();
    if (F != 1u)
        return 0.f;
    const size_t pos = elem_index(row, col, 0u, W, F);
    if (as_float32)
    {
        const float *fp = reinterpret_cast<const float *>(t->data());
        return fp[pos];
    }
    const float zp = t->qp_zp();
    const float sc = t->qp_scale();
    if (t->is_uint16())
    {
        const uint16_t *pu = reinterpret_cast<const uint16_t *>(t->data());
        return (float(pu[pos]) - zp) * sc;
    }
    const uint8_t *p = t->data();
    return (float(p[pos]) - zp) * sc;
}

static float depth_m_from_logit(float logit)
{
    float z = std::exp(-logit);
    z = 1.f / (1.f + z);
    return 1.f / (z * 10.f + 0.009f);
}

static HailoTensorPtr pick_depth_tensor(HailoROIPtr roi, const std::string &want_name)
{
    if (!roi)
        return {};
    if (!want_name.empty())
    {
        HailoTensorPtr t = roi->get_tensor(want_name);
        if (t)
            return t;
    }
    HailoTensorPtr best;
    uint64_t best_hw = 0;
    for (auto &cand : roi->get_tensors())
    {
        if (!cand || cand->is_nms() || cand->features() != 1u)
            continue;
        const uint64_t hw = (uint64_t)cand->height() * (uint64_t)cand->width();
        if (hw > best_hw)
        {
            best_hw = hw;
            best = cand;
        }
    }
    return best;
}

} // namespace

int run(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalDepthResult *out)
{
    if (cfg.type != HAL_POST_TYPE_DEPTH || !out || !roi)
        return HAL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));

    std::string j;
    const char *cj = cfg.config.depth.config_json;
    if (cj && cj[0])
        j.assign(cj);

    std::string want = json_string_after_key(j, "scdepth_output_name");
    if (want.empty())
        want = json_string_after_key(j, "output_tensor_name");
    if (want.empty())
        want = kDefaultTensorName;

    bool as_f32 = false;
    (void)json_bool_after_key(j, "depth_float32", &as_f32);

    HailoTensorPtr t = pick_depth_tensor(roi, want);
    if (!t)
    {
        HAL_LOG_WARNING("hal_internal_scdepth: no depth tensor (tried \"%s\")", want.c_str());
        return HAL_ERR_NOT_READY;
    }

    const uint32_t H = t->height();
    const uint32_t W = t->width();
    if (H < 2u || W < 2u || t->features() != 1u)
    {
        HAL_LOG_WARNING("hal_internal_scdepth: bad tensor shape H=%u W=%u F=%u", H, W, t->features());
        return HAL_ERR_NOT_READY;
    }

    const size_t n = (size_t)H * (size_t)W;
    float *buf = static_cast<float *>(std::malloc(sizeof(float) * n));
    if (!buf)
        return HAL_ERR_NO_MEM;

    for (uint32_t row = 0; row < H; row++)
    {
        for (uint32_t col = 0; col < W; col++)
        {
            const float logit = read_logit(t, row, col, as_f32);
            buf[(size_t)row * (size_t)W + (size_t)col] = depth_m_from_logit(logit);
        }
    }

    out->width = W;
    out->height = H;
    out->depth_m = buf;
    out->priv = nullptr;
    return HAL_OK;
}

} // namespace hal_v2::internal_scdepth

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
