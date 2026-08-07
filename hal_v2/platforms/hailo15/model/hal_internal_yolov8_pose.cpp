/**
 * @file hal_internal_yolov8_pose.cpp
 */

#include "hal_internal_yolov8_pose.hpp"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include "common/hal_common.h"
#include "common/hal_log.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include <hailo_postprocess_tools/objects/hailo_tensors.hpp>

namespace hal_v2::internal_yolov8_pose
{
namespace
{

static constexpr int kRegressionLen = 15;
static constexpr int kNumKpts = 17;
static constexpr int kBoxFeat = 4 * (kRegressionLen + 1);
static constexpr int kKptFeat = kNumKpts * 3;

/** COCO-17 skeleton (same pairs as hailo-apps yolov8pose_postprocess.cpp). */
static const std::array<std::pair<int, int>, 16> kJointPairs = {{
    {0, 1},  {1, 3},  {0, 2},  {2, 4},  {5, 6},  {5, 7},  {7, 9},  {6, 8},
    {8, 10}, {5, 11}, {6, 12}, {11, 12}, {11, 13}, {12, 14}, {13, 15}, {14, 16},
}};

static float json_float_after_key(const std::string &j, const char *key, float def)
{
    if (!key)
        return def;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = j.find(kq);
    if (p == std::string::npos)
        return def;
    p = j.find(':', p);
    if (p == std::string::npos)
        return def;
    p++;
    while (p < j.size() && std::isspace((unsigned char)j[p]))
        p++;
    if (p >= j.size())
        return def;
    char *end = nullptr;
    const float v = std::strtof(j.c_str() + p, &end);
    if (!end || end == j.c_str() + p)
        return def;
    return v;
}

static void softmax_16(float *row16)
{
    float m = row16[0];
    for (int i = 1; i < 16; i++)
        m = std::max(m, row16[i]);
    float s = 0.f;
    for (int i = 0; i < 16; i++)
    {
        row16[i] = std::exp(row16[i] - m);
        s += row16[i];
    }
    if (s > 1e-12f)
    {
        for (int i = 0; i < 16; i++)
            row16[i] /= s;
    }
}

static float iou_xyxy(float ax1, float ay1, float ax2, float ay2, float bx1, float by1, float bx2, float by2)
{
    const float iw = std::min(ax2, bx2) - std::max(ax1, bx1);
    const float ih = std::min(ay2, by2) - std::max(ay1, by1);
    const float inter = std::max(0.f, iw) * std::max(0.f, ih);
    const float aarea = std::max(0.f, ax2 - ax1) * std::max(0.f, ay2 - ay1);
    const float barea = std::max(0.f, bx2 - bx1) * std::max(0.f, by2 - by1);
    const float u = aarea + barea - inter;
    if (u <= 1e-12f)
        return 0.f;
    return inter / u;
}

struct Decoding
{
    float xmin, ymin, xmax, ymax;
    float conf{};
    int class_id{};
    float kx[kNumKpts]{};
    float ky[kNumKpts]{};
    float kscore[kNumKpts]{};
};

static bool tensor_hwc(HailoTensorPtr t, int &H, int &W, int &C)
{
    if (!t || t->is_nms())
        return false;
    /* HailoTensor::shape() is always { height, width, features } (see hailo_tensors.hpp). */
    const auto sh = t->shape();
    if (sh.size() != 3)
        return false;
    H = (int)sh[0];
    W = (int)sh[1];
    C = (int)sh[2];
    return H > 0 && W > 0 && C > 0;
}

enum class PoseTensorKind
{
    Box,
    Score,
    Kpt,
    Unknown,
};

static PoseTensorKind classify_pose_output(int C)
{
    if (C == kBoxFeat)
        return PoseTensorKind::Box;
    if (C == kKptFeat)
        return PoseTensorKind::Kpt;
    /* Score branch: typical person pose is 1 class; allow a small range for multi-class heads. */
    if (C >= 1 && C <= 128)
        return PoseTensorKind::Score;
    return PoseTensorKind::Unknown;
}

struct PoseScaleTensors
{
    int H = 0;
    int W = 0;
    HailoTensorPtr box;
    HailoTensorPtr score;
    HailoTensorPtr kpt;
};

/**
 * HEF output order is not guaranteed to match hailo-apps' [box, score, kpt] × scales.
 * Also @c HailoROI::get_tensors() iterates an internal @c std::map by **tensor name**, not insertion order
 * (see hailo_objects.hpp), so we regroup by (H,W) + channel layout (64 / score / 51) and sort scales by grid
 * area (large → small), matching strides 8/16/32 on square 640 nets.
 */
static std::vector<HailoTensorPtr> order_pose_tensors(const std::vector<HailoTensorPtr> &in, int net_w, int net_h)
{
    std::map<std::pair<int, int>, PoseScaleTensors> buckets;

    for (const auto &t : in)
    {
        if (!t)
            continue;
        int H = 0, W = 0, C = 0;
        if (!tensor_hwc(t, H, W, C))
            continue;
        const PoseTensorKind k = classify_pose_output(C);
        if (k == PoseTensorKind::Unknown)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: skip tensor \"%s\" (H=%d W=%d C=%d) — not box/score/kpt",
                            t->name().c_str(), H, W, C);
            continue;
        }
        const std::pair<int, int> hw{H, W};
        PoseScaleTensors &slot = buckets[hw];
        if (slot.H == 0)
        {
            slot.H = H;
            slot.W = W;
        }
        if (k == PoseTensorKind::Box)
        {
            if (slot.box)
                HAL_LOG_WARNING("hal_internal_yolov8_pose: duplicate box @ %dx%d (keeping \"%s\", also \"%s\")", H, W,
                                slot.box->name().c_str(), t->name().c_str());
            slot.box = t;
        }
        else if (k == PoseTensorKind::Score)
        {
            if (slot.score)
                HAL_LOG_WARNING("hal_internal_yolov8_pose: duplicate score @ %dx%d", H, W);
            slot.score = t;
        }
        else
        {
            if (slot.kpt)
                HAL_LOG_WARNING("hal_internal_yolov8_pose: duplicate kpt @ %dx%d", H, W);
            slot.kpt = t;
        }
    }

    std::vector<std::pair<int, int>> keys;
    keys.reserve(buckets.size());
    for (const auto &e : buckets)
        keys.push_back(e.first);
    std::sort(keys.begin(), keys.end(), [](const std::pair<int, int> &a, const std::pair<int, int> &b) {
        return (int64_t)a.first * (int64_t)a.second > (int64_t)b.first * (int64_t)b.second;
    });

    std::vector<HailoTensorPtr> out;
    for (const auto &hw : keys)
    {
        const PoseScaleTensors &s = buckets.at(hw);
        if (!s.box || !s.score || !s.kpt)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: incomplete scale %dx%d (box=%d score=%d kpt=%d) — skipped",
                            hw.first, hw.second, s.box ? 1 : 0, s.score ? 1 : 0, s.kpt ? 1 : 0);
            continue;
        }
        const int stride_guess = net_w / s.W;
        if (stride_guess <= 0 || stride_guess * s.W != net_w || stride_guess * s.H != net_h)
        {
            HAL_LOG_WARNING(
                "hal_internal_yolov8_pose: scale %dx%d inconsistent with net %dx%d (inferred stride=%d) — skipped",
                s.H, s.W, net_w, net_h, stride_guess);
            continue;
        }
        out.push_back(s.box);
        out.push_back(s.score);
        out.push_back(s.kpt);
    }

    if (!out.empty() && out.size() != in.size())
        HAL_LOG_INFO("hal_internal_yolov8_pose: using %zu / %zu tensors after grouping (extras ignored)",
                     out.size(), in.size());

    if (out.empty())
        HAL_LOG_ERROR("hal_internal_yolov8_pose: could not build box/score/kpt triplets (check C=64 / 51 / score "
                      "channels and net vs grid sizes).");

    return out;
}

/** Same linear index as @c HailoTensor::get / @c get_uint16 (hailo_tensors.hpp). */
static size_t hailo_elem_index(uint32_t row, uint32_t col, uint32_t ch, uint32_t W, uint32_t F)
{
    return (size_t)W * (size_t)F * (size_t)row + (size_t)F * (size_t)col + (size_t)ch;
}

/** Dequant one cell; layout matches hailo-apps @c common::get_xtensor / @c get_xtensor_uint16 + @c dequantize. */
static float dequant_hwc(HailoTensorPtr t, uint32_t row, uint32_t col, uint32_t ch)
{
    if (!t || !t->data())
        return 0.f;
    const uint32_t W = t->width();
    const uint32_t F = t->features();
    const size_t pos = hailo_elem_index(row, col, ch, W, F);
    const float zp = t->qp_zp();
    const float sc = t->qp_scale();
    if (t->is_uint16())
    {
        const uint16_t *p = reinterpret_cast<const uint16_t *>(t->data());
        return (float(p[pos]) - zp) * sc;
    }
    const uint8_t *p = t->data();
    return (float(p[pos]) - zp) * sc;
}

/** Proposal @c j matches xtensor reshape @c (H*W, …): row-major on @c (height, width). */
static void j_to_row_col(int j, uint32_t W, int &row, int &col)
{
    row = j / (int)W;
    col = j % (int)W;
}

static void dequant_box_row(HailoTensorPtr t, int j, float box4x16[4][16])
{
    const uint32_t W = t->width();
    int row = 0, col = 0;
    j_to_row_col(j, W, row, col);
    for (int side = 0; side < 4; side++)
        for (int k = 0; k < 16; k++)
            box4x16[side][k] = dequant_hwc(t, (uint32_t)row, (uint32_t)col, (uint32_t)(side * 16 + k));
}

static void dequant_kpt_block(HailoTensorPtr t, int j, float kreg[kNumKpts][3])
{
    const uint32_t W = t->width();
    int row = 0, col = 0;
    j_to_row_col(j, W, row, col);
    for (int kp = 0; kp < kNumKpts; kp++)
        for (int c = 0; c < 3; c++)
            kreg[kp][c] = dequant_hwc(t, (uint32_t)row, (uint32_t)col, (uint32_t)(kp * 3 + c));
}

/** Class-0 score, same as hailo-apps @c xt::row(scores, instance_index)(0) for @c NUM_CLASSES=1. */
static float dequant_score_ch0(HailoTensorPtr t, int j)
{
    const uint32_t W = t->width();
    int row = 0, col = 0;
    j_to_row_col(j, W, row, col);
    return dequant_hwc(t, (uint32_t)row, (uint32_t)col, 0u);
}

/**
 * Anchor centers aligned with proposal index @c j = row*W + col (same as official @c get_centers flatten).
 */
static std::vector<std::array<float, 4>> build_centers(int stride, uint32_t grid_w, uint32_t grid_h)
{
    const int n = (int)(grid_h * grid_w);
    std::vector<std::array<float, 4>> out((size_t)n);
    for (int j = 0; j < n; j++)
    {
        int row = 0, col = 0;
        j_to_row_col(j, grid_w, row, col);
        const float ct_col = ((float)col + 0.5f) * (float)stride;
        const float ct_row = ((float)row + 0.5f) * (float)stride;
        out[(size_t)j] = {ct_col, ct_row, ct_col, ct_row};
    }
    return out;
}

static std::vector<Decoding> decode_all(const std::vector<HailoTensorPtr> &tensors, int net_w, int net_h,
                                        float score_thr, float nms_iou)
{
    std::vector<Decoding> dec;
    if (tensors.size() < 9 || tensors.size() % 3 != 0)
        return dec;

    const int num_scales = (int)tensors.size() / 3;
    std::vector<std::vector<std::array<float, 4>>> centers_per_scale((size_t)num_scales);
    std::vector<int> proposals_per_scale((size_t)num_scales);
    std::vector<int> stride_per_scale((size_t)num_scales);

    for (int i = 0; i < num_scales; i++)
    {
        int Hb = 0, Wb = 0, Cb = 0;
        if (!tensor_hwc(tensors[(size_t)(i * 3)], Hb, Wb, Cb) || Cb < kBoxFeat)
            return {};
        if (net_w % Wb != 0 || net_h % Hb != 0)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: net %dx%d not divisible by grid %dx%d", net_w, net_h, Hb, Wb);
            return {};
        }
        const int stride = net_w / Wb;
        if (stride * Hb != net_h)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: non-square stride/grid: stride=%d Hb=%d net_h=%d", stride, Hb,
                            net_h);
            return {};
        }
        stride_per_scale[(size_t)i] = stride;
        centers_per_scale[(size_t)i] = build_centers(stride, (uint32_t)Wb, (uint32_t)Hb);
        proposals_per_scale[(size_t)i] = Hb * Wb;
    }

    std::vector<int> score_prefix((size_t)num_scales + 1);
    score_prefix[0] = 0;
    for (int i = 0; i < num_scales; i++)
        score_prefix[(size_t)i + 1] = score_prefix[(size_t)i] + proposals_per_scale[(size_t)i];

    std::vector<float> scores((size_t)score_prefix[(size_t)num_scales]);
    for (int i = 0; i < num_scales; i++)
    {
        HailoTensorPtr ts = tensors[(size_t)(i * 3 + 1)];
        int Hs = 0, Ws = 0, Cs = 0;
        if (!tensor_hwc(ts, Hs, Ws, Cs))
            return {};
        const int nprop_box = proposals_per_scale[(size_t)i];
        if (Hs * Ws != nprop_box)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: score grid %dx%d != box proposals %d", Hs, Ws, nprop_box);
            return {};
        }
        const int nprop = Hs * Ws;
        for (int j = 0; j < nprop; j++)
            scores[(size_t)score_prefix[(size_t)i] + (size_t)j] = dequant_score_ch0(ts, j);
    }

    const float reg_grid[16] = {0.f, 1.f,  2.f,  3.f,  4.f,  5.f,  6.f,  7.f,
                                8.f, 9.f, 10.f, 11.f, 12.f, 13.f, 14.f, 15.f};

    for (int i = 0; i < num_scales; i++)
    {
        HailoTensorPtr tbox = tensors[(size_t)(i * 3)];
        HailoTensorPtr tkpt = tensors[(size_t)(i * 3 + 2)];
        int Hb = 0, Wb = 0, Cb = 0;
        int Hk = 0, Wk = 0, Ck = 0;
        if (!tensor_hwc(tbox, Hb, Wb, Cb) || Cb < kBoxFeat)
            return {};
        if (!tensor_hwc(tkpt, Hk, Wk, Ck) || Ck < kKptFeat)
            return {};
        const int nprop_box = proposals_per_scale[(size_t)i];
        if (Hk * Wk != nprop_box)
        {
            HAL_LOG_WARNING("hal_internal_yolov8_pose: kpt grid %dx%d != box proposals %d", Hk, Wk, nprop_box);
            return {};
        }
        const int nprop = nprop_box;
        const auto &centers = centers_per_scale[(size_t)i];
        if ((int)centers.size() != nprop)
            return {};

        for (int j = 0; j < nprop; j++)
        {
            const float confidence = scores[(size_t)score_prefix[(size_t)i] + (size_t)j];
            if (confidence < score_thr)
                continue;

            float box4x16[4][16];
            dequant_box_row(tbox, j, box4x16);
            for (int r = 0; r < 4; r++)
                softmax_16(box4x16[r]);

            float dist4[4] = {0.f, 0.f, 0.f, 0.f};
            for (int r = 0; r < 4; r++)
            {
                for (int k = 0; k < 16; k++)
                    dist4[r] += box4x16[r][k] * reg_grid[k];
                dist4[r] *= (float)stride_per_scale[(size_t)i];
            }
            const float d0 = -dist4[0];
            const float d1 = -dist4[1];
            const float d2 = dist4[2];
            const float d3 = dist4[3];
            const float cx0 = centers[(size_t)j][0] + d0;
            const float cy0 = centers[(size_t)j][1] + d1;
            const float cx1 = centers[(size_t)j][2] + d2;
            const float cy1 = centers[(size_t)j][3] + d3;

            const float nx0 = cx0 / (float)net_w;
            const float ny0 = cy0 / (float)net_h;
            const float nx1 = cx1 / (float)net_w;
            const float ny1 = cy1 / (float)net_h;

            float kreg[kNumKpts][3];
            dequant_kpt_block(tkpt, j, kreg);
            for (int kp = 0; kp < kNumKpts; kp++)
            {
                kreg[kp][0] *= 2.f;
                kreg[kp][1] *= 2.f;
            }
            const float ctrx = centers[(size_t)j][0];
            const float ctry = centers[(size_t)j][1];

            Decoding d{};
            d.xmin = std::min(nx0, nx1);
            d.ymin = std::min(ny0, ny1);
            d.xmax = std::max(nx0, nx1);
            d.ymax = std::max(ny0, ny1);
            d.conf = confidence;
            d.class_id = 0;

            for (int kp = 0; kp < kNumKpts; kp++)
            {
                const float sx = (float)stride_per_scale[(size_t)i] * (kreg[kp][0] - 0.5f) + ctrx;
                const float sy = (float)stride_per_scale[(size_t)i] * (kreg[kp][1] - 0.5f) + ctry;
                d.kx[kp] = sx / (float)net_w;
                d.ky[kp] = sy / (float)net_h;
                d.kscore[kp] = 1.f / (1.f + std::exp(-kreg[kp][2]));
            }
            dec.push_back(d);
        }
    }

    std::sort(dec.begin(), dec.end(), [](const Decoding &a, const Decoding &b) { return a.conf > b.conf; });

    /* NMS (class-agnostic). */
    for (size_t a = 0; a < dec.size(); a++)
    {
        if (dec[a].conf <= 0.f)
            continue;
        for (size_t b = a + 1; b < dec.size(); b++)
        {
            if (dec[b].conf <= 0.f)
                continue;
            const float iou = iou_xyxy(dec[a].xmin, dec[a].ymin, dec[a].xmax, dec[a].ymax, dec[b].xmin, dec[b].ymin,
                                       dec[b].xmax, dec[b].ymax);
            if (iou >= nms_iou)
                dec[b].conf = 0.f;
        }
    }

    std::vector<Decoding> kept;
    kept.reserve(dec.size());
    for (auto &x : dec)
    {
        if (x.conf > 0.f)
            kept.push_back(x);
    }
    return kept;
}

} // namespace

int run(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalKeypointResult *out)
{
    if (!out || !roi)
        return HAL_ERR_INVALID_ARG;
    std::memset(out, 0, sizeof(*out));

    const auto tensors = roi->get_tensors();
    if (tensors.empty())
    {
        HAL_LOG_WARNING("hal_internal_yolov8_pose: ROI has no tensors");
        return HAL_ERR_NOT_READY;
    }

    int net_w = 640;
    int net_h = 640;
    const char *cj = cfg.config.keypoint.config_json;
    if (cj && cj[0])
    {
        const std::string j(cj);
        const float fw = json_float_after_key(j, "yolov8_pose_network_width", -1.f);
        const float fh = json_float_after_key(j, "yolov8_pose_network_height", -1.f);
        if (fw >= 16.f && fw <= 4096.f)
            net_w = (int)(fw + 0.5f);
        if (fh >= 16.f && fh <= 4096.f)
            net_h = (int)(fh + 0.5f);
    }

    float score_thr = cfg.config.keypoint.confidence_threshold;
    if (cj && cj[0])
    {
        const float st = json_float_after_key(std::string(cj), "score_threshold", NAN);
        if (std::isfinite(st))
            score_thr = st;
    }
    if (!std::isfinite(score_thr) || score_thr < 1e-6f)
        score_thr = 0.6f; /* hailo-apps yolov8pose_postprocess.cpp SCORE_THRESHOLD */
    float nms_iou = 0.7f;
    if (cj && cj[0])
    {
        const float ji = json_float_after_key(std::string(cj), "iou_threshold", NAN);
        if (std::isfinite(ji) && ji > 0.f && ji < 1.f)
            nms_iou = ji;
    }

    std::vector<HailoTensorPtr> ordered = order_pose_tensors(tensors, net_w, net_h);
    if (ordered.empty() || ordered.size() % 3 != 0 || ordered.size() < 9)
    {
        HAL_LOG_WARNING("hal_internal_yolov8_pose: need ≥9 outputs in 3×(box,score,kpt) after grouping; got %zu",
                        ordered.size());
        return HAL_OK;
    }

    std::vector<Decoding> dec = decode_all(ordered, net_w, net_h, score_thr, nms_iou);
    if (dec.empty())
        return HAL_OK;

    const float kpt_thr = std::max(0.f, cfg.config.keypoint.keypoint_threshold);

    out->num_links = 0;
    for (const auto &pr : kJointPairs)
    {
        if (out->num_links >= HAL_MAX_SKELETON_LINKS)
            break;
        HalSkeletonLink L{};
        L.from_idx = pr.first;
        L.to_idx = pr.second;
        L.color = HalColor{0, 255, 255, 255};
        L.thickness = 2.f;
        out->links[out->num_links++] = L;
    }

    for (const auto &d : dec)
    {
        if (out->num_objects >= HAL_MAX_DETECTIONS)
            break;
        HalKeypointObject ko{};
        ko.bbox = HalBBox{d.xmin, d.ymin, std::max(1e-6f, d.xmax - d.xmin), std::max(1e-6f, d.ymax - d.ymin)};
        ko.confidence = d.conf;
        ko.class_id = d.class_id;
        ko.track_id = -1;
        ko.num_keypoints = kNumKpts;
        for (int k = 0; k < kNumKpts; k++)
        {
            if (d.kscore[k] >= kpt_thr)
                ko.keypoints[(uint32_t)k] = HalPoint2D{d.kx[k], d.ky[k], d.kscore[k]};
            else
                ko.keypoints[(uint32_t)k] = HalPoint2D{d.kx[k], d.ky[k], 0.f};
        }
        out->objects[out->num_objects++] = ko;
    }

    return HAL_OK;
}

} // namespace hal_v2::internal_yolov8_pose

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
