/**
 * @file hal_internal_ocr_paddle.cpp
 */

#include "hal_internal_ocr_paddle.hpp"

#if defined(HAL_HAVE_HAILO_POSTPROCESS_TOOLS)

#include "common/hal_common.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

namespace hal_v2::internal_ocr
{
namespace
{

static size_t tensor_num_elements(HailoTensorPtr t)
{
    size_t n = 1;
    if (!t)
        return 0;
    for (auto d : t->shape())
        n *= (size_t)(d > 0 ? d : 1);
    return n;
}

static HailoTensorPtr pick_tensor(HailoROIPtr roi, const std::string &desired)
{
    HailoTensorPtr chosen;
    if (!roi)
        return chosen;
    for (auto &t : roi->get_tensors())
    {
        if (desired.empty())
        {
            chosen = t;
            break;
        }
        if (t->name() == desired)
        {
            chosen = t;
            break;
        }
    }
    if (!chosen && !roi->get_tensors().empty())
        chosen = roi->get_tensors().front();
    return chosen;
}

// Forward decl (pick_det_tensor uses it).
static void infer_det_hw(HailoTensorPtr t, int fallback_h, int fallback_w, int &H, int &W);

static HailoTensorPtr pick_det_tensor(HailoROIPtr roi, const std::string &desired, int fallback_h, int fallback_w)
{
    if (!roi)
        return {};
    if (!desired.empty())
        return pick_tensor(roi, desired);

    // Heuristic: prefer a tensor that yields the largest inferred H*W.
    HailoTensorPtr best;
    uint64_t best_hw = 0;
    for (auto &t : roi->get_tensors())
    {
        if (!t)
            continue;
        int H = fallback_h;
        int W = fallback_w;
        infer_det_hw(t, fallback_h, fallback_w, H, W);
        if (H <= 0 || W <= 0)
            continue;
        const uint64_t hw = (uint64_t)H * (uint64_t)W;
        if (hw > best_hw)
        {
            best_hw = hw;
            best = t;
        }
    }
    if (best)
        return best;
    return pick_tensor(roi, desired);
}

/** Paddle-style spatial dims from tensor shape (ocr_postprocess.cpp paddleocr_det). */
static void infer_det_hw(HailoTensorPtr t, int fallback_h, int fallback_w, int &H, int &W)
{
    H = fallback_h;
    W = fallback_w;
    if (!t)
        return;
    const auto &sh = t->shape();
    if (sh.size() == 4)
    {
        if (sh[1] == 1)
        {
            H = int(sh[2]);
            W = int(sh[3]);
        }
        else if (sh[3] == 1)
        {
            H = int(sh[1]);
            W = int(sh[2]);
        }
        else
        {
            H = int(sh[2]);
            W = int(sh[3]);
        }
    }
    else if (sh.size() == 3)
    {
        if (sh[2] == 1)
        {
            H = int(sh[0]);
            W = int(sh[1]);
        }
        else if (sh[0] == 1)
        {
            H = int(sh[1]);
            W = int(sh[2]);
        }
        else
        {
            std::vector<int> v{int(sh[0]), int(sh[1]), int(sh[2])};
            std::sort(v.begin(), v.end());
            H = v[1];
            W = v[2];
        }
    }
    else if (sh.size() == 2)
    {
        H = int(sh[0]);
        W = int(sh[1]);
    }
    if (W <= 4 && H > 16)
        std::swap(H, W);
}

/** Same idea as ocr_example_v2 simple_det_connected_components_from_prob_u8. */
static void det_connected_components_prob_u8(const uint8_t *prob_u8, uint32_t H, uint32_t W, float bin_thresh,
                                           float box_thresh, float unclip_ratio, float min_box_size_px,
                                           uint32_t max_candidates, uint32_t min_area, std::vector<HalDetection> &out)
{
    out.clear();
    if (!prob_u8 || H == 0 || W == 0)
        return;
    const uint8_t thr = (uint8_t)std::max(0, std::min(255, (int)std::lround(bin_thresh * 255.0f)));

    const uint32_t N = H * W;
    std::vector<int32_t> visited;
    visited.assign(N, 0);
    std::vector<uint32_t> q;
    q.reserve(4096);

    auto push_det = [&](uint32_t minx, uint32_t miny, uint32_t maxx, uint32_t maxy, uint32_t area, uint64_t sum_u8) {
        if (area < min_area)
            return;
        const float mean = (area > 0) ? (float)sum_u8 / (float)area : 0.0f;
        const float score = mean / 255.0f;
        if (score < box_thresh)
            return;
        const float bw = (float)(maxx - minx + 1U);
        const float bh = (float)(maxy - miny + 1U);
        if (bw < min_box_size_px || bh < min_box_size_px)
            return;

        float expand = std::max(1.0f, unclip_ratio);
        const float dx = 0.5f * (expand - 1.0f) * bw;
        const float dy = 0.5f * (expand - 1.0f) * bh;
        int32_t ex0 = (int32_t)std::floor((float)minx - dx);
        int32_t ey0 = (int32_t)std::floor((float)miny - dy);
        int32_t ex1 = (int32_t)std::ceil((float)maxx + dx);
        int32_t ey1 = (int32_t)std::ceil((float)maxy + dy);
        ex0 = std::max<int32_t>(0, ex0);
        ey0 = std::max<int32_t>(0, ey0);
        ex1 = std::min<int32_t>((int32_t)W - 1, ex1);
        ey1 = std::min<int32_t>((int32_t)H - 1, ey1);
        if (ex1 <= ex0 || ey1 <= ey0)
            return;

        HalDetection d{};
        d.bbox.x = (float)ex0 / (float)W;
        d.bbox.y = (float)ey0 / (float)H;
        d.bbox.w = (float)(ex1 - ex0 + 1) / (float)W;
        d.bbox.h = (float)(ey1 - ey0 + 1) / (float)H;
        d.confidence = score;
        d.class_id = 0;
        d.track_id = -1;
        std::snprintf(d.label, sizeof(d.label), "%s", "text_region");
        out.push_back(d);
    };

    for (uint32_t y0 = 0; y0 < H; y0++)
    {
        for (uint32_t x0 = 0; x0 < W; x0++)
        {
            const uint32_t idx0 = y0 * W + x0;
            if (visited[idx0])
                continue;
            if (prob_u8[idx0] < thr)
            {
                visited[idx0] = 1;
                continue;
            }
            uint32_t minx = x0, miny = y0, maxx = x0, maxy = y0;
            uint32_t area = 0;
            uint64_t sum_u8 = 0;
            q.clear();
            q.push_back(idx0);
            visited[idx0] = 1;
            for (size_t qi = 0; qi < q.size(); qi++)
            {
                const uint32_t idx = q[qi];
                const uint32_t y = idx / W;
                const uint32_t x = idx % W;
                area++;
                sum_u8 += prob_u8[idx];
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > maxx) maxx = x;
                if (y > maxy) maxy = y;
                auto try_push = [&](int32_t nx, int32_t ny) {
                    if (nx < 0 || ny < 0 || nx >= (int32_t)W || ny >= (int32_t)H)
                        return;
                    const uint32_t nidx = (uint32_t)ny * W + (uint32_t)nx;
                    if (visited[nidx])
                        return;
                    visited[nidx] = 1;
                    if (prob_u8[nidx] >= thr)
                        q.push_back(nidx);
                };
                try_push((int32_t)x - 1, (int32_t)y);
                try_push((int32_t)x + 1, (int32_t)y);
                try_push((int32_t)x, (int32_t)y - 1);
                try_push((int32_t)x, (int32_t)y + 1);
            }
            push_det(minx, miny, maxx, maxy, area, sum_u8);
            if (out.size() >= HAL_MAX_DETECTIONS)
                return;
        }
    }

    if (max_candidates > 0 && out.size() > max_candidates)
    {
        std::partial_sort(out.begin(), out.begin() + (size_t)max_candidates, out.end(),
                          [](const HalDetection &a, const HalDetection &b) { return a.confidence > b.confidence; });
        out.resize((size_t)max_candidates);
    }
}

static void softmax1d(std::vector<float> &v)
{
    float m = *std::max_element(v.begin(), v.end());
    double sum = 0.0;
    for (float &x : v)
        sum += std::exp(double(x - m));
    for (float &x : v)
        x = float(std::exp(double(x - m)) / sum);
}

static void fill_default_charset(std::vector<std::string> &charset)
{
    charset.clear();
    charset.emplace_back("blank");
    for (char c = '0'; c <= '9'; ++c)
        charset.emplace_back(1, c);
    charset.emplace_back(":");
    charset.emplace_back(";");
    charset.emplace_back("<");
    charset.emplace_back("=");
    charset.emplace_back(">");
    charset.emplace_back("?");
    charset.emplace_back("@");
    for (char c = 'A'; c <= 'Z'; ++c)
        charset.emplace_back(1, c);
    charset.emplace_back("[");
    charset.emplace_back("\\");
    charset.emplace_back("]");
    charset.emplace_back("^");
    charset.emplace_back("_");
    charset.emplace_back("`");
    for (char c = 'a'; c <= 'z'; ++c)
        charset.emplace_back(1, c);
    charset.emplace_back("{");
    charset.emplace_back("|");
    charset.emplace_back("}");
    charset.emplace_back("~");
    charset.emplace_back("!");
    charset.emplace_back("\"");
    charset.emplace_back("#");
    charset.emplace_back("$");
    charset.emplace_back("%");
    charset.emplace_back("&");
    charset.emplace_back("'");
    charset.emplace_back("(");
    charset.emplace_back(")");
    charset.emplace_back("*");
    charset.emplace_back("+");
    charset.emplace_back(",");
    charset.emplace_back("-");
    charset.emplace_back(".");
    charset.emplace_back("/");
    charset.emplace_back(" ");
    charset.emplace_back(" ");
}

/** Best-effort bbox around max activation when CC yields no regions (aligns with ocr_example_v2 helper). */
static void det_bbox_from_prob_u8(const uint8_t *prob_u8, uint32_t H, uint32_t W, float bin_thresh,
                                  std::vector<HalDetection> &out)
{
    out.clear();
    if (!prob_u8 || H == 0 || W == 0)
        return;
    uint8_t maxv = 0;
    uint32_t maxx = 0, maxy = 0;
    for (uint32_t i = 0; i < H * W; i++)
    {
        if (prob_u8[i] > maxv)
        {
            maxv = prob_u8[i];
            maxy = i / W;
            maxx = i % W;
        }
    }

    uint8_t thr = (uint8_t)std::max(0, std::min(255, (int)std::lround(bin_thresh * 255.0f)));
    if (maxv > 0 && thr > maxv)
        thr = (uint8_t)std::max<uint8_t>(1, (uint8_t)((uint32_t)maxv * 7U / 10U));

    bool any = false;
    uint32_t minx = W, miny = H, bx1 = 0, by1 = 0;
    for (uint32_t y = 0; y < H; y++)
    {
        const uint8_t *row = prob_u8 + (size_t)y * (size_t)W;
        for (uint32_t x = 0; x < W; x++)
        {
            if (row[x] >= thr)
            {
                any = true;
                if (x < minx) minx = x;
                if (y < miny) miny = y;
                if (x > bx1) bx1 = x;
                if (y > by1) by1 = y;
            }
        }
    }
    if (!any || minx >= W || miny >= H)
    {
        if (maxv == 0)
            return;
        const uint32_t half_w = std::max(8U, W / 10U);
        const uint32_t half_h = std::max(4U, H / 10U);
        minx = (maxx > half_w) ? (maxx - half_w) : 0U;
        miny = (maxy > half_h) ? (maxy - half_h) : 0U;
        bx1 = std::min(W - 1U, maxx + half_w);
        by1 = std::min(H - 1U, maxy + half_h);
    }

    const uint32_t pad_x = std::max(1U, W / 100U);
    const uint32_t pad_y = std::max(1U, H / 100U);
    minx = (minx > pad_x) ? (minx - pad_x) : 0U;
    miny = (miny > pad_y) ? (miny - pad_y) : 0U;
    bx1 = std::min(W - 1U, bx1 + pad_x);
    by1 = std::min(H - 1U, by1 + pad_y);

    HalDetection d{};
    d.bbox.x = (float)minx / (float)W;
    d.bbox.y = (float)miny / (float)H;
    d.bbox.w = (float)(bx1 - minx + 1U) / (float)W;
    d.bbox.h = (float)(by1 - miny + 1U) / (float)H;
    d.confidence = 0.99f;
    d.class_id = 0;
    d.track_id = -1;
    std::snprintf(d.label, sizeof(d.label), "%s", "text_region");
    out.push_back(d);
}

} // namespace

void load_recognition_charset(const HalOcrRecognitionPostConfig &cfg, std::vector<std::string> &charset_out)
{
    charset_out.clear();
    if (!cfg.charset_path[0])
    {
        fill_default_charset(charset_out);
        return;
    }
    std::ifstream in(cfg.charset_path);
    if (!in.is_open())
    {
        HAL_LOG_WARNING("hal_internal_ocr: failed to open charset_path \"%s\", using default charset", cfg.charset_path);
        fill_default_charset(charset_out);
        return;
    }
    std::string line;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            charset_out.push_back(line);
    }
    if (charset_out.empty())
        fill_default_charset(charset_out);
}

int run_detection(const HalPostprocessConfig &cfg, HailoROIPtr roi, HalDetectionResult *out)
{
    if (cfg.type != HAL_POST_TYPE_OCR_DETECTION || !out || !roi)
        return HAL_ERR_INVALID_ARG;
    out->num_detections = 0;
    out->priv = nullptr;

    const HalOcrDetectionPostConfig &dc = cfg.config.ocr_detection;
    std::string want_name(dc.det_output_name);
    HailoTensorPtr t = pick_det_tensor(roi, want_name, dc.det_map_h, dc.det_map_w);
    if (!t)
    {
        HAL_LOG_WARNING("hal_internal_ocr: detection ROI has no tensors");
        return HAL_ERR_NOT_READY;
    }

    int H = dc.det_map_h;
    int W = dc.det_map_w;
    infer_det_hw(t, dc.det_map_h, dc.det_map_w, H, W);
    if (H <= 0 || W <= 0)
        return HAL_ERR_NOT_READY;

    const uint8_t *u8 = reinterpret_cast<const uint8_t *>(t->data());
    if (!u8)
        return HAL_ERR_NOT_READY;
    const size_t need = (size_t)H * (size_t)W;
    /* Some builds pad C dimension in the reported shape; raw buffer is at least H*W for prob map. */
    if (tensor_num_elements(t) < need)
    {
        HAL_LOG_WARNING("hal_internal_ocr: det tensor element count too small (%zu < %zu)", tensor_num_elements(t), need);
        return HAL_ERR_NOT_READY;
    }

    const uint32_t min_area = (uint32_t)std::max(16U, (uint32_t)(H * W) / 2000U);
    std::vector<HalDetection> tmp;
    det_connected_components_prob_u8(u8, (uint32_t)H, (uint32_t)W, dc.det_bin_thresh, dc.det_box_thresh,
                                       dc.det_unclip_ratio, dc.det_min_box_size,
                                       (uint32_t)std::max(1, dc.det_max_candidates), min_area, tmp);
    if (tmp.empty())
        det_bbox_from_prob_u8(u8, (uint32_t)H, (uint32_t)W, dc.det_bin_thresh, tmp);

    const float min_c = dc.min_confidence;
    uint32_t n = 0;
    for (const auto &d : tmp)
    {
        if (min_c > 0.f && d.confidence < min_c)
            continue;
        if (n >= HAL_MAX_DETECTIONS)
            break;
        out->detections[n++] = d;
    }
    out->num_detections = n;
    return HAL_OK;
}

int run_recognition(const HalPostprocessConfig &cfg, HailoROIPtr roi, const std::vector<std::string> &charset,
                    HalOcrResult *out)
{
    if (cfg.type != HAL_POST_TYPE_OCR_RECOGNITION || !out || !roi)
        return HAL_ERR_INVALID_ARG;
    out->num_lines = 0;
    out->priv = nullptr;

    const HalOcrRecognitionPostConfig &rc = cfg.config.ocr_recognition;
    std::string want_name(rc.rec_output_name);
    HailoTensorPtr t = pick_tensor(roi, want_name);
    if (!t)
        return HAL_ERR_NOT_READY;

    const auto &shape = t->shape();
    if (shape.size() != 3)
    {
        HAL_LOG_WARNING("hal_internal_ocr: recognition tensor rank=%zu (expected 3)", shape.size());
        return HAL_ERR_NOT_READY;
    }

    const uint8_t *u8 = reinterpret_cast<const uint8_t *>(t->data());
    if (!u8)
        return HAL_ERR_NOT_READY;

    const size_t N = (size_t)shape[0];
    const size_t D1 = (size_t)shape[1];
    const size_t D2 = (size_t)shape[2];
    if (N != 1 || D1 == 0 || D2 == 0)
        return HAL_ERR_NOT_READY;

    size_t C = 0, T = 0;
    bool layout_is_NCT = rc.time_major;
    if (layout_is_NCT)
    {
        C = D1;
        T = D2;
    }
    else
    {
        T = D1;
        C = D2;
    }

    /* Logical C / row stride: prob rows may be padded vs reported shape (stride = elements / T). */
    size_t logical_C = C;
    const size_t total_u8 = tensor_num_elements(t);
    size_t stride_row = C;
    if (T > 0 && total_u8 % T == 0)
    {
        const size_t sr = total_u8 / T;
        if (sr > 0)
        {
            stride_row = sr;
            if (sr < logical_C)
                logical_C = sr;
        }
    }

    std::vector<std::vector<float>> probs(T, std::vector<float>(logical_C));
    if (layout_is_NCT)
    {
        for (size_t c = 0; c < logical_C; ++c)
        {
            for (size_t t0 = 0; t0 < T; ++t0)
            {
                float v = u8[c * T + t0] * (1.0f / 255.0f);
                probs[t0][c] = v;
            }
        }
    }
    else
    {
        for (size_t t0 = 0; t0 < T; ++t0)
        {
            for (size_t c = 0; c < logical_C; ++c)
                probs[t0][c] = u8[t0 * stride_row + c] * (1.0f / 255.0f);
        }
    }

    if (!rc.logits_are_softmax)
    {
        for (size_t t0 = 0; t0 < T; ++t0)
            softmax1d(probs[t0]);
    }

    std::vector<int> text_index(T);
    std::vector<float> text_prob(T);
    for (size_t t0 = 0; t0 < T; ++t0)
    {
        auto &row = probs[t0];
        auto it = std::max_element(row.begin(), row.end());
        text_index[t0] = int(std::distance(row.begin(), it));
        text_prob[t0] = *it;
    }

    std::vector<bool> selection(T, true);
    for (size_t i = 1; i < T; ++i)
        selection[i] = (text_index[i] != text_index[i - 1]);
    const int blank_idx = rc.blank_index;
    for (size_t i = 0; i < T; ++i)
    {
        if (text_index[i] == blank_idx)
            selection[i] = false;
    }

    std::string text;
    text.reserve(T);
    std::vector<float> conf_list;
    conf_list.reserve(T);
    for (size_t i = 0; i < T; ++i)
    {
        if (!selection[i])
            continue;
        int idx = text_index[i];
        const int dict_idx = idx - rc.charset_index_offset;
        if (dict_idx >= 0 && (size_t)dict_idx < charset.size())
        {
            text += charset[(size_t)dict_idx];
            conf_list.push_back(text_prob[i]);
        }
        else
        {
            text += "?";
            conf_list.push_back(0.f);
        }
    }

    float conf = 0.f;
    if (!conf_list.empty())
    {
        float s = 0.f;
        for (float c : conf_list)
            s += c;
        conf = s / (float)conf_list.size();
    }

    if (text.empty() || text == " ")
        return HAL_ERR_NOT_READY;

    /* Smooth confidence with previous-style factor (optional, simple moving average on single line — no-op). */
    (void)rc.text_conf_smooth;

    out->num_lines = 1;
    HalOcrLine &ln = out->lines[0];
    ln.bbox = HalBBox{0.f, 0.f, 1.f, 1.f};
    ln.track_id = -1;
    ln.confidence = conf;
    std::snprintf(ln.text, sizeof(ln.text), "%s", text.c_str());
    if (rc.min_confidence > 0.f && ln.confidence < rc.min_confidence)
    {
        out->num_lines = 0;
        return HAL_ERR_NOT_READY;
    }
    return HAL_OK;
}

} // namespace hal_v2::internal_ocr

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
