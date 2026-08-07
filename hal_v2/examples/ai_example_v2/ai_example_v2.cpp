/**
 * @file ai_example_v2.cpp
 * @brief Example: Media (manual feed) -> (optional DSP resize/crop) -> Inference -> Postprocess -> Draw -> Encode -> RTP/UDP.
 *
 * Key points:
 * - Select media profile by name or index.
 * - Disable encoder auto-feed; app feeds encoder after drawing.
 * - Pick AI input stream closest to model input; if mismatch, resize via DSP (NV12 DMABUF).
 * - Optional: CLIP prompts via HalClipPostprocessConfig + hal_clip_postprocess_config_merge_json() on `--post-json`
 *   (keys: prompts[], positive_prompt, negative_prompts[], score_threshold, match_policy; see hal_postprocess.h).
 * - Optional: face-landmarks two-model mode (detector -> crop -> landmarks).
 *
 * Usage (minimal):
 *   hal-ai-example-v2 --media <medialib_json> --profile <name_or_idx> --model <model.hef> --udp <host:port>
 *
 * Receiver:
 *   See `hal_v2/examples/udp_stream_test/receive_h264_rtp.sdp` / `receive_h265_rtp.sdp`
 */
#include "common/hal_clip_prompt_scorer.hpp"
#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"

// Media
#include "media/hal_codec_internal.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"

// DSP
#include "dsp/hal_dsp.h"

// AI
#include "model/hal_inference.h"
#include "model/hal_postprocess.h"
#include "model/hal_draw.h"
#include "model/hal_clip_text_encoder.hpp"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <unistd.h>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <algorithm>
#include <numeric>

namespace
{

static std::atomic<bool> g_stop{false};
static volatile sig_atomic_t g_sig = 0;
static std::atomic<uint64_t> g_frames_in{0};
static std::atomic<uint64_t> g_ai_frames_in{0};
static std::atomic<uint64_t> g_ai_infer_ok{0};
static std::atomic<uint64_t> g_ai_post_ok{0};
static std::atomic<uint64_t> g_frames_encoded_in{0};
static std::atomic<uint64_t> g_pkts_out{0};

static void on_sig(int) { g_sig = 1; }

enum class PreprocessColorChoice : uint8_t
{
    Auto,
    None,
    Nv12ToRgb,
    Nv12ToBgr,
};

struct CliState
{
    std::mutex mu;
    std::string clip_prompts_json; // inline JSON for HAL_POST_TYPE_CLIP
    bool request_reload_post = false;
};

static std::string extract_json_string_key_best_effort(const std::string &json, const char *key)
{
    if (!key)
        return {};
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = json.find(kq);
    if (p == std::string::npos)
        return {};
    p = json.find(':', p);
    if (p == std::string::npos)
        return {};
    p++;
    while (p < json.size() && std::isspace((unsigned char)json[p]))
        p++;
    if (p >= json.size() || json[p] != '"')
        return {};
    p++;
    const size_t q = json.find('"', p);
    if (q == std::string::npos)
        return {};
    return json.substr(p, q - p);
}

static HalClipMatchPolicyKind clip_policy_kind_from_cpp(hal_v2::HalClipMatchPolicy p)
{
    if (p == hal_v2::HalClipMatchPolicy::Margin)
        return HAL_CLIP_MATCH_MARGIN;
    if (p == hal_v2::HalClipMatchPolicy::PosOnly)
        return HAL_CLIP_MATCH_POS_ONLY;
    return HAL_CLIP_MATCH_SOFTMAX;
}

static float extract_json_float_key_best_effort(const std::string &json, const char *key, float default_val)
{
    if (!key)
        return default_val;
    const std::string kq = std::string("\"") + key + "\"";
    size_t p = json.find(kq);
    if (p == std::string::npos)
        return default_val;
    p = json.find(':', p);
    if (p == std::string::npos)
        return default_val;
    p++;
    while (p < json.size() && std::isspace((unsigned char)json[p]))
        p++;
    if (p >= json.size())
        return default_val;
    char *end = nullptr;
    const float v = std::strtof(json.c_str() + p, &end);
    if (!end || end == json.c_str() + p)
        return default_val;
    return v;
}

struct FrameJob
{
    void *video_ctx = nullptr;
    HalFrameBuffer *frame = nullptr; // heap-cloned frame, must be released via HAL_VIDEO_OPS.release_frame + delete
};

struct SharedFrame
{
    void *video_ctx = nullptr;
    HalFrameBuffer fb{};
    std::atomic<int> refs{0};
};

static SharedFrame *sharedframe_from_callback(void *video_ctx, HalFrameBuffer *stack_frame, int initial_refs)
{
    if (!video_ctx || !stack_frame || initial_refs <= 0)
        return nullptr;
    auto *sf = new (std::nothrow) SharedFrame{};
    if (!sf)
        return nullptr;
    sf->video_ctx = video_ctx;
    sf->fb = *stack_frame;
    stack_frame->priv = nullptr;
    sf->refs.store(initial_refs, std::memory_order_release);
    return sf;
}

static void sharedframe_unref(SharedFrame *sf)
{
    if (!sf)
        return;
    const int prev = sf->refs.fetch_sub(1, std::memory_order_acq_rel);
    if (prev == 1)
    {
        (void)HAL_VIDEO_OPS.release_frame(sf->video_ctx, &sf->fb);
        delete sf;
    }
}

struct SharedResult
{
    std::mutex mu;
    std::condition_variable cv;
    HalPostprocessResult last{};
    uint64_t seq = 0;
    bool valid = false;

    HalPostprocessResult last_valid{};
    uint64_t last_valid_seq = 0;
    bool has_valid = false;
    std::chrono::steady_clock::time_point last_valid_tp{};
};

struct ClipPromptState
{
    std::mutex mu;
    bool enabled = false;
    hal_v2::HalClipTextEncoder text;
    hal_v2::HalClipPromptScorer scorer;
    HalClipPostprocessConfig clip_cfg{};

    ClipPromptState() : scorer(text) {}
};

enum class DpmRenderMode
{
    Mosaic,
    Blur,
    Overlay,
};

// One per-ROI segmentation mask for the attach_frame_analytics path. The bytemask is small
// (e.g. 256x256); the media library blender scales it to the ROI bbox on the encoded frame.
struct DpmSegRoi
{
    float x, y, w, h; // normalized bbox on the displayed frame [0..1]
    uint32_t mw, mh;  // bytemask pixel dims
    std::vector<uint8_t> mask; // mw*mh bytes; 0=transparent, 255=mask
    char label[HAL_PM_LABEL_LEN];
};

struct AppCtx
{
    // media
    void *media_ctx = nullptr;
    void *video_ctx = nullptr; // legacy (preview)
    void *ai_video_ctx = nullptr;
    void *codec_ctx = nullptr;
    std::string stream_key; // legacy (preview)
    std::string ai_stream_key;

    // dsp
    void *dsp_ctx = nullptr;

    // ai
    bool two_model_landmarks = false;
    bool verbose = false;
    PreprocessColorChoice preprocess_color_choice = PreprocessColorChoice::Auto;
    bool cls_softmax = false;
    uint32_t cls_top_k = 5;
    /** Optional ImageNet-style `imagenet_class_index.json`: index -> human-readable name (second field). */
    std::vector<std::string> cls_label_table;
    HalInferenceSession *infer = nullptr;
    HalPostprocessSession *post = nullptr;
    HalPostprocessConfig post_cfg{};
    HalDrawConfig draw_cfg{};

    HalInferenceSession *det_infer = nullptr;
    HalPostprocessSession *det_post = nullptr;
    uint32_t det_w = 0;
    uint32_t det_h = 0;

    // DPM (Dynamic Privacy Mask) mode: detector -> ROI crop -> segmentation -> aggregate -> render
    struct
    {
        bool enabled = false;
        std::string labels_csv = "person,vehicle";
        uint32_t max_rois = 35;
        float smooth_alpha = 0.5f;
        uint32_t mask_size = 128;
        uint32_t mosaic_block_size = 24; // 0 => blur; larger default aligns better with HW pixelization
        DpmRenderMode render_mode = DpmRenderMode::Mosaic;

        // DPM outputs shared with preview thread.
        std::mutex mu;
        uint32_t last_w = 0;
        uint32_t last_h = 0;
        std::vector<HalDrawMosaic> last_rois; // rectangles to mosaic/blur
        uint64_t last_masked_pixels = 0;
        uint32_t overlay_log_throttle = 0;

        // Per-ROI segmentation masks for the attach_frame_analytics path. Each entry is one
        // detection ROI's bytemask (small, e.g. 256x256) — the blender scales it to the ROI bbox.
        // This matches the media library blender's design (small per-ROI masks, NOT a full-frame mask).
        std::vector<DpmSegRoi> seg_rois;
        uint64_t seg_rois_seq = 0; // bumped whenever seg_rois is refreshed
        bool recttest = false; // --dpm-attach-recttest: fill content region solid (ignore model) to isolate DSP placement

        // Persistent full-frame mask for smoothing (0..255 per pixel).
        std::vector<uint8_t> smoothed_mask_u8;
        bool have_smoothed = false;
        uint64_t mask_seq = 0;

        // Scaled-to-preview copy (when AI stream dims != preview stream dims).
        uint32_t scaled_w = 0;
        uint32_t scaled_h = 0;
        std::vector<uint8_t> scaled_mask_u8;
        uint64_t scaled_from_seq = 0;
        uint32_t scale_log_throttle = 0;

        // Hardware privacy mask path (MediaLibrary/encoder masking).
        bool hw_privacy_mask = true; // default ON for mosaic/blur (DSP privacy mask driven by segmentation bitmask)
        bool use_attach_api = false; // --dpm-attach: drive the media blender dynamic path via attach_frame_analytics
        char attach_label[HAL_PM_LABEL_LEN]{"person"}; // first label from labels_csv, cached for the per-frame attach call
        uint64_t hw_applied_seq = 0;
        uint32_t hw_log_throttle = 0;
        std::vector<HalPrivacyMaskItem> hw_items;
        std::vector<std::string> hw_ids;

        // DSP privacy mask bitmask (4x4 quantized, 1 bit per block) + ROI hints in bitmask space.
        uint32_t bm_w = 0; // in 4x4 blocks
        uint32_t bm_h = 0;
        uint32_t bm_stride = 0; // bytes
        std::vector<uint8_t> bm_bits;
        std::vector<HalDspRoi> bm_rois;
    } dpm;

    // udp
    HalUdpStream *udp = nullptr;

    // model input size
    uint32_t model_w = 0;
    uint32_t model_h = 0;

    // queues
    std::mutex q_mu;
    std::condition_variable q_cv;
    std::deque<SharedFrame *> q;
    static constexpr size_t kQMax = 4;
    std::thread ai_worker;

    std::mutex preview_mu;
    std::condition_variable preview_cv;
    std::deque<SharedFrame *> preview_q;
    std::thread preview_worker;

    SharedResult result;
    ClipPromptState clip_prompt;

    /** EMA-smoothed CLIP scores for preview OSD only (raw values stay in SharedResult). */
    static constexpr uint32_t kClipOsdEmaSlots = 16u;
    float clip_osd_ema[kClipOsdEmaSlots]{};
    uint32_t clip_osd_ema_num_classes = 0;

    // keep encoder input buffers alive until packets drain
    std::mutex inflight_mu;
    std::deque<HalFrameBuffer *> inflight;
    static constexpr size_t kInflightMax = 8;

    CliState *cli_state = nullptr;
};

static void enqueue_ai(AppCtx *ctx, SharedFrame *sf)
{
    if (!ctx || !sf)
        return;
    std::unique_lock<std::mutex> lk(ctx->q_mu);
    while (ctx->q.size() >= AppCtx::kQMax && !g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *drop = ctx->q.front();
        ctx->q.pop_front();
        lk.unlock();
        sharedframe_unref(drop);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        sharedframe_unref(sf);
        return;
    }
    ctx->q.push_back(sf);
    ctx->q_cv.notify_one();
}

static void enqueue_preview(AppCtx *ctx, SharedFrame *sf)
{
    if (!ctx || !sf)
        return;
    std::unique_lock<std::mutex> lk(ctx->preview_mu);
    while (ctx->preview_q.size() >= AppCtx::kQMax && !g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *drop = ctx->preview_q.front();
        ctx->preview_q.pop_front();
        lk.unlock();
        sharedframe_unref(drop);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        sharedframe_unref(sf);
        return;
    }
    ctx->preview_q.push_back(sf);
    ctx->preview_cv.notify_one();
}

static void video_cb_preview_and_ai(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !frame) return;
    g_frames_in.fetch_add(1, std::memory_order_relaxed);
    g_ai_frames_in.fetch_add(1, std::memory_order_relaxed);

    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 2);
    if (!sf)
        return;
    enqueue_preview(ctx, sf);
    enqueue_ai(ctx, sf);
}

static void video_cb_preview_only(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !frame) return;
    g_frames_in.fetch_add(1, std::memory_order_relaxed);
    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 1);
    if (!sf)
        return;
    enqueue_preview(ctx, sf);
}

static void video_cb_ai_only(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !frame) return;
    g_frames_in.fetch_add(1, std::memory_order_relaxed);
    g_ai_frames_in.fetch_add(1, std::memory_order_relaxed);
    SharedFrame *sf = sharedframe_from_callback(video_ctx, frame, 1);
    if (!sf)
        return;
    enqueue_ai(ctx, sf);
}

static void codec_cb(void *codec_ctx, HalPacketBuffer *packet, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !packet) return;
    if (ctx->udp && ctx->udp->ok() && packet->data && packet->size > 0)
    {
        ctx->udp->push_annex_b(packet->data, packet->size, packet->timestamp_ns);
        g_pkts_out.fetch_add(1, std::memory_order_relaxed);
    }

    (void)HAL_CODEC_OPS.release_packet(codec_ctx, packet);

    // best-effort release one in-flight input per output packet
    HalFrameBuffer *to_free = nullptr;
    {
        std::lock_guard<std::mutex> lk(ctx->inflight_mu);
        if (!ctx->inflight.empty())
        {
            to_free = ctx->inflight.front();
            ctx->inflight.pop_front();
        }
    }
    if (to_free)
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(to_free);
}

static std::optional<std::pair<uint32_t, uint32_t>> model_input_hw(HalInferenceSession *s)
{
    if (!s) return std::nullopt;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK || mi.num_inputs == 0)
        return std::nullopt;

    // Prefer common NHWC image tensor shapes. Heuristic: pick first 4D tensor.
    for (uint32_t i = 0; i < mi.num_inputs; i++)
    {
        const auto &in = mi.inputs[i];
        if (in.ndim >= 4)
        {
            const int32_t h = in.shape[1];
            const int32_t w = in.shape[2];
            if (w > 0 && h > 0)
                return std::make_pair((uint32_t)w, (uint32_t)h);
        }
    }
    return std::nullopt;
}

static bool model_prefers_tensor_from_frame(HalInferenceSession *s)
{
    if (!s)
        return false;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK)
        return false;
    // NV12 planar models typically expose 2 inputs (Y + UV). For those, we keep the explicit NV12 path.
    // Most RGB/BGR models expose a single input tensor; use tensor_from_frame() for preprocessing support.
    return (mi.num_inputs <= 1);
}

static PreprocessColorChoice preprocess_choice_from_str(const std::string &s)
{
    if (s == "auto")
        return PreprocessColorChoice::Auto;
    if (s == "none")
        return PreprocessColorChoice::None;
    if (s == "nv12_to_rgb")
        return PreprocessColorChoice::Nv12ToRgb;
    if (s == "nv12_to_bgr")
        return PreprocessColorChoice::Nv12ToBgr;
    // Backward/alias spellings
    if (s == "nv12-to-rgb")
        return PreprocessColorChoice::Nv12ToRgb;
    if (s == "nv12-to-bgr")
        return PreprocessColorChoice::Nv12ToBgr;
    return PreprocessColorChoice::Auto;
}

static HalPreprocessColor choose_preprocess_color_auto(HalInferenceSession *s)
{
    if (!s)
        return HAL_PREPROCESS_COLOR_NONE;
    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(s, &mi) != HAL_OK || mi.num_inputs == 0)
        return HAL_PREPROCESS_COLOR_NONE;

    // If backend knows it's NV12, avoid conversion.
    if (mi.num_inputs == 1 && mi.inputs[0].is_nv12)
        return HAL_PREPROCESS_COLOR_NONE;

    // Heuristic: if model exposes a single non-NV12 input, default to NV12->RGB.
    // (Override with --preprocess-color nv12_to_bgr when the model expects BGR.)
    if (mi.num_inputs == 1 && !mi.inputs[0].is_nv12)
        return HAL_PREPROCESS_COLOR_NV12_TO_RGB;

    return HAL_PREPROCESS_COLOR_NONE;
}

static HalPreprocessConfig make_preprocess_cfg(HalPreprocessColor c)
{
    HalPreprocessConfig p{};
    p.color = c;
    p.resize = HAL_PREPROCESS_RESIZE_BILINEAR;
    p.letterbox = HAL_PREPROCESS_LETTERBOX_NONE;
    p.pad_value = 0;
    p.normalize = false;
    p.mean[0] = p.mean[1] = p.mean[2] = p.mean[3] = 0.0f;
    p.std[0] = p.std[1] = p.std[2] = p.std[3] = 1.0f;
    p.output_layout = HAL_TENSOR_LAYOUT_UNKNOWN;
    return p;
}

static void softmax_inplace(std::vector<float> &v)
{
    if (v.empty())
        return;
    float mx = v[0];
    for (float x : v)
        mx = std::max(mx, x);
    double sum = 0.0;
    for (float &x : v)
    {
        x = std::exp(x - mx);
        sum += x;
    }
    const double denom = (sum > 0.0) ? sum : 1.0;
    for (float &x : v)
        x = (float)(x / denom);
}

static std::string read_entire_file_cstr(const char *path)
{
    if (!path || path[0] == '\0')
        return {};
    std::FILE *f = std::fopen(path, "rb");
    if (!f)
        return {};
    if (std::fseek(f, 0, SEEK_END) != 0)
    {
        std::fclose(f);
        return {};
    }
    long sz = std::ftell(f);
    if (sz <= 0)
    {
        std::fclose(f);
        return {};
    }
    if (std::fseek(f, 0, SEEK_SET) != 0)
    {
        std::fclose(f);
        return {};
    }
    std::string s;
    s.resize((size_t)sz);
    const size_t nread = std::fread(s.data(), 1, (size_t)sz, f);
    std::fclose(f);
    if (nread != (size_t)sz)
        s.resize(nread);
    return s;
}

static void skip_json_ws(const std::string &json, size_t &p)
{
    while (p < json.size() && std::isspace((unsigned char)json[p]))
        ++p;
}

static bool read_json_string_token(const std::string &json, size_t &p, std::string &out)
{
    skip_json_ws(json, p);
    if (p >= json.size() || json[p] != '"')
        return false;
    ++p;
    const size_t a = p;
    while (p < json.size() && json[p] != '"')
    {
        if (json[p] == '\\')
        {
            ++p;
            if (p < json.size())
                ++p;
            continue;
        }
        ++p;
    }
    if (p >= json.size())
        return false;
    out = json.substr(a, p - a);
    ++p;
    return true;
}

static bool load_imagenet_class_index_json(const char *path, std::vector<std::string> &out_labels)
{
    out_labels.assign(1000, std::string{});
    const std::string json = read_entire_file_cstr(path);
    if (json.empty())
        return false;

    for (int id = 0; id < 1000; ++id)
    {
        const std::string key = std::string("\"") + std::to_string(id) + "\":";
        size_t p = json.find(key);
        if (p == std::string::npos)
            continue;
        p += key.size();
        skip_json_ws(json, p);
        if (p >= json.size() || json[p] != '[')
            continue;
        ++p;

        std::string synset;
        if (!read_json_string_token(json, p, synset))
            continue;
        skip_json_ws(json, p);
        if (p >= json.size() || json[p] != ',')
            continue;
        ++p;

        std::string human;
        if (!read_json_string_token(json, p, human))
            continue;
        out_labels[(size_t)id] = std::move(human);
    }
    return true;
}

static void copy_to_hal_label(char *dst, size_t dstsz, const std::string &s)
{
    if (!dst || dstsz == 0)
        return;
    if (s.empty())
    {
        dst[0] = '\0';
        return;
    }
    const size_t n = std::min(s.size(), dstsz - 1U);
    std::memcpy(dst, s.c_str(), n);
    dst[n] = '\0';
}

static bool synthesize_classification_from_raw_output(AppCtx *ctx,
                                                      const HalTensor *outputs, int num_outputs,
                                                      HalPostprocessResult &out_pr)
{
    if (!ctx || !ctx->infer || !outputs || num_outputs <= 0)
        return false;

    // Expect one output vector (e.g. 1x1x1000) in UINT8/UINT16/F32; Hailo backend presents it as a flat blob.
    const HalTensor &t0 = outputs[0];
    if (!t0.data || t0.byte_size == 0)
        return false;

    HalModelInfo mi{};
    if (HAL_INFERENCE_OPS.get_model_info(ctx->infer, &mi) != HAL_OK || mi.num_outputs == 0)
        return false;

    const auto &o0 = mi.outputs[0];
    const float scale = std::isfinite(o0.quant_scale) ? o0.quant_scale : 1.0f;
    const float zp = std::isfinite(o0.quant_zero_point) ? o0.quant_zero_point : 0.0f;

    size_t n = 0;
    std::vector<float> scores;
    if (t0.dtype == HAL_DTYPE_UINT8)
    {
        n = t0.byte_size;
        scores.resize(n);
        const uint8_t *p = static_cast<const uint8_t *>(t0.data);
        for (size_t i = 0; i < n; i++)
            scores[i] = (float)(((float)p[i] - zp) * scale);
    }
    else if (t0.dtype == HAL_DTYPE_UINT16)
    {
        n = t0.byte_size / 2;
        scores.resize(n);
        const uint16_t *p = static_cast<const uint16_t *>(t0.data);
        for (size_t i = 0; i < n; i++)
            scores[i] = (float)(((float)p[i] - zp) * scale);
    }
    else if (t0.dtype == HAL_DTYPE_FLOAT32)
    {
        n = t0.byte_size / 4;
        scores.resize(n);
        const float *p = static_cast<const float *>(t0.data);
        for (size_t i = 0; i < n; i++)
            scores[i] = p[i];
    }
    else
    {
        return false;
    }

    if (n == 0)
        return false;

    if (ctx->cls_softmax)
        softmax_inplace(scores);

    // Top-k
    const uint32_t k = std::min<uint32_t>(ctx->cls_top_k ? ctx->cls_top_k : 5U, (uint32_t)std::min<size_t>(n, HAL_MAX_CLASSES));
    std::vector<size_t> idx(n);
    std::iota(idx.begin(), idx.end(), 0);
    std::partial_sort(idx.begin(), idx.begin() + k, idx.end(),
                      [&](size_t a, size_t b) { return scores[a] > scores[b]; });

    std::memset(&out_pr, 0, sizeof(out_pr));
    out_pr.type = HAL_POST_TYPE_CLASSIFICATION;
    auto &cr = out_pr.result.classification;
    cr.num_classes = k;
    cr.top1_class_id = (k > 0) ? (int32_t)idx[0] : -1;
    for (uint32_t i = 0; i < k; i++)
    {
        const int32_t cid = (int32_t)idx[i];
        cr.classes[i].class_id = cid;
        if (cid >= 0 && (size_t)cid < ctx->cls_label_table.size() && !ctx->cls_label_table[(size_t)cid].empty())
            copy_to_hal_label(cr.classes[i].label, sizeof(cr.classes[i].label), ctx->cls_label_table[(size_t)cid]);
        else
            std::snprintf(cr.classes[i].label, sizeof(cr.classes[i].label), "class_%d", cid);
        cr.classes[i].confidence = scores[idx[i]];
        cr.classes[i].type[0] = '\0';
    }
    cr.priv = nullptr;
    out_pr.priv = nullptr;
    return true;
}

static HalPixelFormat pixfmt_from_preprocess_choice(PreprocessColorChoice c)
{
    if (c == PreprocessColorChoice::Nv12ToBgr)
        return HAL_PIX_FMT_BGR24;
    if (c == PreprocessColorChoice::Nv12ToRgb)
        return HAL_PIX_FMT_RGB24;
    return HAL_PIX_FMT_NV12;
}

static bool build_packed_rgb_inputs_from_frame(const HalFrameBuffer *frame, HalTensor *inputs, int &num_inputs_out)
{
    num_inputs_out = 0;
    if (!frame || !inputs)
        return false;
    if ((frame->format != HAL_PIX_FMT_RGB24 && frame->format != HAL_PIX_FMT_BGR24) ||
        frame->num_planes < 1 || !frame->planes[0])
        return false;

    HalTensor t{};
    t.data = (void *)frame->planes[0];
    t.ndim = 3;
    t.shape[0] = (int32_t)frame->height;
    t.shape[1] = (int32_t)frame->width;
    t.shape[2] = 3;
    t.dtype = HAL_DTYPE_UINT8;
    t.byte_size = frame->sizes[0] ? frame->sizes[0] : (uint32_t)(frame->strides[0] * frame->height);
    t.dma_fd = frame->dma_fds[0];
    t.priv = frame->priv;

    inputs[0] = t;
    num_inputs_out = 1;
    return true;
}

static uint32_t pick_video_index_closest(void **video_list, uint32_t count, uint32_t mw, uint32_t mh)
{
    if (!video_list || count == 0) return UINT32_MAX;
    uint32_t best = 0;
    uint64_t best_cost = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v) continue;
        const uint64_t dw = (v->config.width > mw) ? (v->config.width - mw) : (mw - v->config.width);
        const uint64_t dh = (v->config.height > mh) ? (v->config.height - mh) : (mh - v->config.height);
        const uint64_t cost = dw + dh;
        if (cost < best_cost)
        {
            best_cost = cost;
            best = i;
        }
    }
    return best;
}

static uint32_t pick_video_index_exact_or_closest(void **video_list, uint32_t count, uint32_t w, uint32_t h)
{
    if (!video_list || count == 0)
        return UINT32_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (v && v->config.width == w && v->config.height == h)
            return i;
    }
    return pick_video_index_closest(video_list, count, w, h);
}

static uint32_t pick_video_index_exact_or_closest_prefer_not(void **video_list, uint32_t count,
                                                             uint32_t w, uint32_t h,
                                                             uint32_t avoid_index)
{
    if (!video_list || count == 0)
        return UINT32_MAX;

    // 1) Exact match, prefer not avoid_index.
    uint32_t any_exact = UINT32_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v || v->config.width != w || v->config.height != h)
            continue;
        if (any_exact == UINT32_MAX)
            any_exact = i;
        if (i != avoid_index)
            return i;
    }
    if (any_exact != UINT32_MAX)
        return any_exact;

    // 2) Closest, prefer not avoid_index.
    uint32_t best = UINT32_MAX;
    uint64_t best_cost = UINT64_MAX;
    uint32_t best_any = UINT32_MAX;
    uint64_t best_any_cost = UINT64_MAX;
    for (uint32_t i = 0; i < count; i++)
    {
        auto *v = static_cast<HalVideoContext *>(video_list[i]);
        if (!v) continue;
        const uint64_t dw = (v->config.width > w) ? (v->config.width - w) : (w - v->config.width);
        const uint64_t dh = (v->config.height > h) ? (v->config.height - h) : (h - v->config.height);
        const uint64_t cost = dw + dh;
        if (cost < best_any_cost)
        {
            best_any_cost = cost;
            best_any = i;
        }
        if (i != avoid_index && cost < best_cost)
        {
            best_cost = cost;
            best = i;
        }
    }
    return (best != UINT32_MAX) ? best : best_any;
}

static inline uint32_t ceil_div_u32(uint32_t a, uint32_t b)
{
    return (a + b - 1U) / b;
}

static inline uint32_t align_up_even_u32(uint32_t v)
{
    return (v + 1U) & ~1U;
}

/**
 * HailoDSP constraint: dst width/height can't be more than 16x smaller than src.
 * For large downscale (e.g. 3840 -> 192), do a multi-stage resize chain:
 *   src -> intermediate_1 -> ... -> dst
 *
 * @return HAL_OK on success. On failure, all intermediates are released.
 */
static int dsp_resize_chain(void *dsp_ctx, const HalFrameBuffer *src, HalFrameBuffer *dst,
                            HalDspInterpolation interpolation,
                            std::vector<HalFrameBuffer *> &intermediates_out)
{
    intermediates_out.clear();
    if (!dsp_ctx || !src || !dst)
        return HAL_ERR_INVALID_ARG;

    constexpr uint32_t kMaxDownscale = 16U;
    const HalFrameBuffer *cur_src = src;

    // Build intermediate steps until within DSP downscale limit.
    while (cur_src->width > dst->width * kMaxDownscale || cur_src->height > dst->height * kMaxDownscale)
    {
        const uint32_t next_w = std::max(dst->width, ceil_div_u32(cur_src->width, kMaxDownscale));
        const uint32_t next_h = std::max(dst->height, ceil_div_u32(cur_src->height, kMaxDownscale));

        HalFrameBufferRequest req{};
        req.width = next_w;
        req.height = next_h;
        req.format = HAL_PIX_FMT_NV12;
        req.mem_type = HAL_MEM_DMABUF;
        req.zero_initialize = false;

        HalFrameBuffer *mid = nullptr;
        int arc = HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &mid);
        if (arc != HAL_OK || !mid)
            goto fail;

        HalDspResizeParams rp{};
        rp.src = cur_src;
        rp.dst = mid;
        rp.interpolation = interpolation;

        int rc = HAL_DSP_OPS.resize(dsp_ctx, &rp);
        if (rc != HAL_OK)
        {
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(mid);
            goto fail;
        }

        intermediates_out.push_back(mid);
        cur_src = mid;
    }

    // Final step to dst.
    {
        HalDspResizeParams rp{};
        rp.src = cur_src;
        rp.dst = dst;
        rp.interpolation = interpolation;
        int rc = HAL_DSP_OPS.resize(dsp_ctx, &rp);
        if (rc != HAL_OK)
            goto fail;
    }

    return HAL_OK;

fail:
    for (auto *b : intermediates_out)
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
    intermediates_out.clear();
    return HAL_ERR_CHECK;
}

static void ml_clone_metadata_best_effort(const HalFrameBuffer *src, HalFrameBuffer *dst)
{
    if (!src || !dst)
        return;
    (void)HAL_FRAME_BUFFER_OPS.copy_metadata_from_frame_buffer(src, dst);
}

static inline uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static bool bbox_to_roi(const HalDetection &d, uint32_t frame_w, uint32_t frame_h, HalDspRoi &roi_out)
{
    if (frame_w == 0 || frame_h == 0)
        return false;
    const float x0 = d.bbox.x;
    const float y0 = d.bbox.y;
    const float x1 = d.bbox.x + d.bbox.w;
    const float y1 = d.bbox.y + d.bbox.h;
    if (!(x1 > x0 && y1 > y0))
        return false;

    // Convert normalized bbox to pixel ROI.
    uint32_t sx = clamp_u32((uint32_t)std::floor(x0 * (float)frame_w), 0U, frame_w);
    uint32_t sy = clamp_u32((uint32_t)std::floor(y0 * (float)frame_h), 0U, frame_h);
    uint32_t ex = clamp_u32((uint32_t)std::ceil(x1 * (float)frame_w), 0U, frame_w);
    uint32_t ey = clamp_u32((uint32_t)std::ceil(y1 * (float)frame_h), 0U, frame_h);

    // NV12/YUV420 requires even width/height after crop. Align ROI to even boundaries.
    // ROI uses [start, end) (end is exclusive), so making both start/end even guarantees even sizes.
    sx &= ~1U;
    sy &= ~1U;
    ex = (ex + 1U) & ~1U;
    ey = (ey + 1U) & ~1U;

    // Clamp again after alignment.
    sx = clamp_u32(sx, 0U, frame_w);
    sy = clamp_u32(sy, 0U, frame_h);
    ex = clamp_u32(ex, 0U, frame_w);
    ey = clamp_u32(ey, 0U, frame_h);

    // Ensure non-empty ROI; if we collapsed, try to expand minimally (still even-sized).
    if (ex <= sx)
    {
        if (sx + 2U <= frame_w) ex = sx + 2U;
        else if (sx >= 2U) sx -= 2U, ex = sx + 2U;
    }
    if (ey <= sy)
    {
        if (sy + 2U <= frame_h) ey = sy + 2U;
        else if (sy >= 2U) sy -= 2U, ey = sy + 2U;
    }
    if (ex <= sx || ey <= sy)
        return false;
    roi_out.start_x = sx;
    roi_out.start_y = sy;
    roi_out.end_x = ex;
    roi_out.end_y = ey;
    return true;
}

static inline float clamp_f(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi) ? hi : v;
}

static void expand_face_bbox_norm(float bbox_x, float bbox_y, float bbox_w, float bbox_h,
                                  float margin, float &out_x, float &out_y, float &out_w, float &out_h)
{
    // Same intent as v1: add some context around the face crop.
    const float mx = bbox_w * margin;
    const float my = bbox_h * margin;
    float x0 = bbox_x - mx;
    float y0 = bbox_y - my;
    float x1 = bbox_x + bbox_w + mx;
    float y1 = bbox_y + bbox_h + my;
    x0 = clamp_f(x0, 0.0f, 1.0f);
    y0 = clamp_f(y0, 0.0f, 1.0f);
    x1 = clamp_f(x1, 0.0f, 1.0f);
    y1 = clamp_f(y1, 0.0f, 1.0f);
    out_x = x0;
    out_y = y0;
    out_w = std::max(0.0f, x1 - x0);
    out_h = std::max(0.0f, y1 - y0);
}

static bool bbox_norm_to_roi(float x, float y, float w, float h,
                             uint32_t frame_w, uint32_t frame_h, HalDspRoi &roi_out)
{
    HalDetection d{};
    d.bbox = HalBBox{x, y, w, h};
    return bbox_to_roi(d, frame_w, frame_h, roi_out);
}

static void remap_keypoints_to_frame(HalPostprocessResult &pr,
                                     const HalDspRoi &roi,
                                     uint32_t frame_w, uint32_t frame_h,
                                     uint32_t crop_w, uint32_t crop_h)
{
    if (pr.type != HAL_POST_TYPE_KEYPOINT)
        return;
    auto &out = pr.result.keypoint;
    const float roi_w = (roi.end_x > roi.start_x) ? (float)(roi.end_x - roi.start_x) : 0.0f;
    const float roi_h = (roi.end_y > roi.start_y) ? (float)(roi.end_y - roi.start_y) : 0.0f;
    if (roi_w <= 1.0f || roi_h <= 1.0f)
        return;

    // Heuristic: if points are in [0..1], treat as normalized; otherwise treat as pixel in crop space.
    float max_x = 0.0f, max_y = 0.0f;
    for (uint32_t oi = 0; oi < out.num_objects; oi++)
    {
        const auto &obj = out.objects[oi];
        for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
        {
            max_x = std::max(max_x, obj.keypoints[ki].x);
            max_y = std::max(max_y, obj.keypoints[ki].y);
        }
    }
    const bool normalized = (max_x <= 2.0f && max_y <= 2.0f);
    const float sx = normalized ? roi_w : (roi_w / (float)std::max<uint32_t>(1U, crop_w));
    const float sy = normalized ? roi_h : (roi_h / (float)std::max<uint32_t>(1U, crop_h));

    for (uint32_t oi = 0; oi < out.num_objects; oi++)
    {
        auto &obj = out.objects[oi];
        obj.bbox = HalBBox{
            (float)roi.start_x / (float)std::max<uint32_t>(1U, frame_w),
            (float)roi.start_y / (float)std::max<uint32_t>(1U, frame_h),
            roi_w / (float)std::max<uint32_t>(1U, frame_w),
            roi_h / (float)std::max<uint32_t>(1U, frame_h),
        };
        for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
        {
            float x = obj.keypoints[ki].x;
            float y = obj.keypoints[ki].y;
            if (normalized)
            {
                x = clamp_f(x, 0.0f, 1.0f);
                y = clamp_f(y, 0.0f, 1.0f);
            }
            const float fx = (float)roi.start_x + x * sx;
            const float fy = (float)roi.start_y + y * sy;
            obj.keypoints[ki].x = fx / (float)std::max<uint32_t>(1U, frame_w);
            obj.keypoints[ki].y = fy / (float)std::max<uint32_t>(1U, frame_h);
        }
    }
}

static bool build_nv12_inputs_from_frame(const HalFrameBuffer *frame, HalTensor *inputs, int &num_inputs_out)
{
    num_inputs_out = 0;
    if (!frame || !inputs)
        return false;
    if (frame->format != HAL_PIX_FMT_NV12 || frame->num_planes < 2 || !frame->planes[0] || !frame->planes[1])
        return false;

    // Input0: Y plane
    HalTensor y{};
    y.data = (void *)frame->planes[0];
    y.ndim = 2;
    y.shape[0] = (int32_t)frame->height;
    y.shape[1] = (int32_t)frame->width;
    y.dtype = HAL_DTYPE_UINT8;
    y.byte_size = frame->sizes[0] ? frame->sizes[0] : (uint32_t)(frame->strides[0] * frame->height);
    y.dma_fd = frame->dma_fds[0];
    y.priv = frame->priv;

    // Input1: UV plane (H/2 x W)
    HalTensor uv{};
    uv.data = (void *)frame->planes[1];
    uv.ndim = 2;
    uv.shape[0] = (int32_t)(frame->height / 2);
    uv.shape[1] = (int32_t)frame->width;
    uv.dtype = HAL_DTYPE_UINT8;
    uv.byte_size = frame->sizes[1] ? frame->sizes[1] : (uint32_t)(frame->strides[1] * (frame->height / 2));
    uv.dma_fd = frame->dma_fds[1];
    uv.priv = frame->priv;

    inputs[0] = y;
    inputs[1] = uv;
    num_inputs_out = 2;
    return true;
}

static void sanitize_postprocess_result(HalPostprocessResult &pr)
{
    pr.priv = nullptr;
    switch (pr.type)
    {
        case HAL_POST_TYPE_DETECTION:
        case HAL_POST_TYPE_OCR_DETECTION:
            pr.result.detection.priv = nullptr;
            break;
        case HAL_POST_TYPE_CLASSIFICATION:
        case HAL_POST_TYPE_CLIP:
            pr.result.classification.priv = nullptr;
            break;
        case HAL_POST_TYPE_KEYPOINT:
            pr.result.keypoint.priv = nullptr;
            break;
        case HAL_POST_TYPE_SEGMENTATION:
            pr.result.segmentation.priv = nullptr;
            break;
        case HAL_POST_TYPE_DEPTH:
            pr.result.depth.priv = nullptr;
            break;
        case HAL_POST_TYPE_EMBEDDING:
            pr.result.embedding.priv = nullptr;
            break;
        case HAL_POST_TYPE_OCR_RECOGNITION:
            pr.result.ocr.priv = nullptr;
            break;
        default:
            break;
    }
}

static void publish_result(AppCtx *ctx, HalPostprocessResult &pr)
{
    if (!ctx)
        return;
    sanitize_postprocess_result(pr);
    std::lock_guard<std::mutex> lk(ctx->result.mu);
    ctx->result.last = pr;
    ctx->result.valid = true;
    ctx->result.seq++;
    if (pr.type != HAL_POST_TYPE_NONE)
    {
        bool ok = true;
        if (pr.type == HAL_POST_TYPE_KEYPOINT)
            ok = (pr.result.keypoint.num_objects > 0);
        else if (pr.type == HAL_POST_TYPE_DETECTION || pr.type == HAL_POST_TYPE_OCR_DETECTION)
            ok = (pr.result.detection.num_detections > 0);
        else if (pr.type == HAL_POST_TYPE_OCR_RECOGNITION)
            ok = (pr.result.ocr.num_lines > 0);
        else if (pr.type == HAL_POST_TYPE_DEPTH)
            ok = (pr.result.depth.depth_m != nullptr && pr.result.depth.width > 0u && pr.result.depth.height > 0u);
        if (ok)
        {
            ctx->result.last_valid = pr;
            ctx->result.last_valid_seq = ctx->result.seq;
            ctx->result.has_valid = true;
            ctx->result.last_valid_tp = std::chrono::steady_clock::now();
        }
    }
    ctx->result.cv.notify_all();
}

static std::vector<std::string> split_csv_trim(const std::string &csv);

static void l2_normalize_vector_inplace(std::vector<float> &v)
{
    double sum = 0.0;
    for (float x : v)
        sum += (double)x * (double)x;
    if (sum <= 1e-18)
        return;
    const float inv = (float)(1.0 / std::sqrt(sum));
    for (float &x : v)
        x *= inv;
}

/** When postprocess returns an image embedding and CLIP text state is enabled, build a CLIP classification result for OSD. */
static HalPostprocessResult synthesize_clip_from_embedding_if_enabled(AppCtx *ctx, const HalPostprocessResult &pr)
{
    if (pr.type != HAL_POST_TYPE_EMBEDDING || !ctx->clip_prompt.enabled)
        return pr;

    std::lock_guard<std::mutex> lk(ctx->clip_prompt.mu);
    const auto &er = pr.result.embedding;
    if (er.dim == 0)
        return pr;

    std::vector<float> img(er.dim);
    for (uint32_t i = 0; i < er.dim; i++)
        img[i] = er.data[i];
    // Match text side (HalClipTextEncoder L2-normalized): cosine = dot after both are unit vectors.
    l2_normalize_vector_inplace(img);

    hal_v2::HalClipPromptScorer &sc = ctx->clip_prompt.scorer;
    if (!sc.ready())
        return pr;

    if (sc.positive_negative_mode())
    {
        hal_v2::HalClipScoreResult sr{};
        const int rc = sc.score_normalized_image(img, sr);
        if (rc != HAL_OK)
            return pr;
        HalPostprocessResult syn{};
        sc.fill_clip_classification_result(sr, syn);
        return syn;
    }

    HalPostprocessResult syn{};
    if (sc.score_zero_shot_top1(img, syn) == HAL_OK)
        return syn;
    return pr;
}

static void scale_mask_u8_nn(const uint8_t *src, uint32_t sw, uint32_t sh,
                             uint8_t *dst, uint32_t dw, uint32_t dh);

static void dpm_apply_hw_privacy_mask(AppCtx *ctx, HalFrameBuffer *frame);
static void dpm_apply_attach_privacy_mask(AppCtx *ctx, HalFrameBuffer *frame);

static void dpm_build_dsp_bitmask_and_rois_locked(AppCtx *ctx, uint32_t frame_w, uint32_t frame_h);

static void ai_worker_loop(AppCtx *ctx)
{
    if (!ctx || !ctx->codec_ctx || !ctx->dsp_ctx || !ctx->infer)
        return;

    while (!g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *sf = nullptr;
        {
            std::unique_lock<std::mutex> lk(ctx->q_mu);
            ctx->q_cv.wait(lk, [&] { return g_stop.load(std::memory_order_acquire) || !ctx->q.empty(); });
            if (g_stop.load(std::memory_order_acquire))
                break;
            sf = ctx->q.front();
            ctx->q.pop_front();
        }
        if (!sf)
            continue;
        FrameJob job{};
        job.video_ctx = sf->video_ctx;
        job.frame = &sf->fb;

        // AI path:
        // - single-model: resize (if needed) -> infer -> post -> draw
        // - two-model landmarks: detector -> bbox -> crop+resize -> landmarks -> post -> draw

        HalFrameBuffer *ai_resized = nullptr;
        std::vector<HalFrameBuffer *> ai_resize_chain;

        if (!ctx->two_model_landmarks)
        {
            if (ctx->dpm.enabled)
            {
                // DPM stage 1: run detector and publish filtered detections (segmentation comes later).
                if (ctx->det_infer && ctx->det_post && ctx->det_w && ctx->det_h)
                {
                    HalFrameBuffer *det_in = nullptr;
                    std::vector<HalFrameBuffer *> det_chain;

                    // Resize full frame -> detector input
                    if (job.frame->width == ctx->det_w && job.frame->height == ctx->det_h)
                    {
                        det_in = const_cast<HalFrameBuffer *>(job.frame);
                    }
                    else
                    {
                        HalFrameBufferRequest dreq{};
                        dreq.width = ctx->det_w;
                        dreq.height = ctx->det_h;
                        dreq.format = HAL_PIX_FMT_NV12;
                        dreq.mem_type = HAL_MEM_DMABUF;
                        dreq.zero_initialize = false;
                        if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&dreq, &det_in) == HAL_OK && det_in)
                        {
                            ml_clone_metadata_best_effort(job.frame, det_in);
                            if (dsp_resize_chain(ctx->dsp_ctx, job.frame, det_in, HAL_DSP_INTERPOLATION_BILINEAR, det_chain) != HAL_OK)
                            {
                                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
                                det_in = nullptr;
                            }
                        }
                    }

                    HalPostprocessResult dpr{};
                    if (det_in)
                    {
                        HalTensor din{};
                        HalTensor din2[2]{};
                        int dnin = 1;
                        HalTensor dout[HAL_MAX_TENSORS]{};
                        int dnum_out = 0;
                        uint32_t dwant_in = 1;
                        {
                            HalModelInfo dmi{};
                            if (HAL_INFERENCE_OPS.get_model_info(ctx->det_infer, &dmi) == HAL_OK)
                            {
                                dnum_out = (int)((dmi.num_outputs <= HAL_MAX_TENSORS) ? dmi.num_outputs : HAL_MAX_TENSORS);
                                dwant_in = dmi.num_inputs ? dmi.num_inputs : 1U;
                            }
                        }
                        int dirc = HAL_ERR_NOT_SUPPORTED;
                        if (dwant_in >= 2 && build_nv12_inputs_from_frame(det_in, din2, dnin))
                        {
                            dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, din2, dnin, dout, dnum_out);
                        }
                        else if (HAL_INFERENCE_OPS.tensor_from_frame(det_in, &din) == HAL_OK)
                        {
                            dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, &din, 1, dout, dnum_out);
                            HAL_INFERENCE_OPS.free_tensor(&din);
                        }
                        if (dirc == HAL_OK && dnum_out > 0)
                        {
                            (void)HAL_POSTPROCESS_OPS.run(ctx->det_post, dout, dnum_out, &dpr);
                        }

                        for (int i = 0; i < dnum_out; i++)
                            if (dout[i].data)
                                HAL_INFERENCE_OPS.free_tensor(&dout[i]);
                    }

                    if (dpr.type == HAL_POST_TYPE_DETECTION || dpr.type == HAL_POST_TYPE_OCR_DETECTION)
                    {
                        const auto allow = split_csv_trim(ctx->dpm.labels_csv);
                        HalPostprocessResult out = dpr;
                        auto &dr = out.result.detection;
                        // Filter by label allowlist (empty allowlist => keep all), then sort by confidence.
                        HalDetection tmp[HAL_MAX_DETECTIONS]{};
                        uint32_t n = 0;
                        for (uint32_t i = 0; i < dr.num_detections && n < HAL_MAX_DETECTIONS; i++)
                        {
                            const HalDetection &dd = dr.detections[i];
                            bool ok = allow.empty();
                            if (!ok)
                            {
                                for (const auto &lbl : allow)
                                {
                                    if (dd.label[0] && lbl == dd.label)
                                    {
                                        ok = true;
                                        break;
                                    }
                                }
                            }
                            if (ok)
                                tmp[n++] = dd;
                        }
                        std::sort(tmp, tmp + n, [](const HalDetection &a, const HalDetection &b) { return a.confidence > b.confidence; });
                        const uint32_t keep = std::min<uint32_t>(n, ctx->dpm.max_rois ? ctx->dpm.max_rois : 35U);
                        dr.num_detections = keep;
                        for (uint32_t i = 0; i < keep; i++)
                            dr.detections[i] = tmp[i];

                        // DPM stage 2: ROI crop -> segmentation -> aggregate mask.
                        const uint32_t fw = job.frame->width;
                        const uint32_t fh = job.frame->height;
                        std::vector<uint8_t> mask_u8;
                        mask_u8.assign((size_t)fw * (size_t)fh, 0);
                        std::vector<HalDrawMosaic> rois;
                        rois.reserve(dr.num_detections);
                        std::vector<DpmSegRoi> local_seg_rois; // per-ROI masks for the attach path
                        local_seg_rois.reserve(dr.num_detections);
                        uint64_t masked_pixels_total = 0;
                        uint64_t seg_nonzero_total = 0;
                        uint8_t seg_min = 255, seg_max = 0;

                        const uint32_t dst_w = ctx->model_w ? ctx->model_w : ctx->dpm.mask_size;
                        const uint32_t dst_h = ctx->model_h ? ctx->model_h : ctx->dpm.mask_size;

                        for (uint32_t di = 0; di < dr.num_detections; di++)
                        {
                            HalDspRoi roi{};
                            if (!bbox_to_roi(dr.detections[di], fw, fh, roi))
                                continue;
                            const uint32_t roi_w = roi.end_x - roi.start_x;
                            const uint32_t roi_h = roi.end_y - roi.start_y;
                            if (roi_w < 2 || roi_h < 2)
                                continue;

                            rois.push_back(HalDrawMosaic{(int32_t)roi.start_x, (int32_t)roi.start_y, (int32_t)roi_w, (int32_t)roi_h,
                                                         (int32_t)ctx->dpm.mosaic_block_size});

                            HalFrameBuffer *seg_in = nullptr;
                            HalFrameBufferRequest sreq{};
                            sreq.width = dst_w;
                            sreq.height = dst_h;
                            sreq.format = HAL_PIX_FMT_NV12;
                            sreq.mem_type = HAL_MEM_DMABUF;
                            sreq.zero_initialize = false;
                            if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&sreq, &seg_in) != HAL_OK || !seg_in)
                                continue;
                            ml_clone_metadata_best_effort(job.frame, seg_in);

                            HalDspCropResizeParams cp{};
                            cp.src = job.frame;
                            cp.dst = seg_in;
                            cp.crop = roi;
                            cp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
                            // STRETCH the ROI to fill the seg input. The seg model is stretch-trained
                            // (it outputs the person filling 256x256 regardless of letterbox input), so
                            // a letterbox crop would make it output a shifted/over-filled mask. The
                            // attach path resamples the stretched mask back into the letterbox content
                            // region (matching the blender's letterbox contract) before feeding.
                            cp.scaling_mode = HAL_DSP_SCALING_STRETCH;
                            cp.letterbox_alignment = HAL_DSP_LETTERBOX_MIDDLE;
                            cp.letterbox_color = HalDspColor{.y = 0, .u = 128, .v = 128};
                            if (HAL_DSP_OPS.crop_and_resize(ctx->dsp_ctx, &cp) != HAL_OK)
                            {
                                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(seg_in);
                                continue;
                            }

                            // Diag (throttled): mmap seg_in Y plane and sample corner vs center to
                            // verify letterbox is applied (corner should be 0/black, center non-zero).
                            if (ctx->dpm.use_attach_api && (ctx->dpm.overlay_log_throttle++ % 60) == 0)
                            {
                                if (seg_in->dma_fds[0] >= 0 && seg_in->strides[0] > 0)
                                {
                                    void *p = mmap(nullptr, seg_in->sizes[0], PROT_READ, MAP_SHARED, seg_in->dma_fds[0], 0);
                                    if (p && p != MAP_FAILED)
                                    {
                                        const uint8_t *y = (const uint8_t *)p;
                                        const uint32_t st = seg_in->strides[0];
                                        HAL_LOG_INFO("ai_example_v2: seg_in letterbox probe Y(0,0)=%u Y(%u,%u)=%u (dst=%ux%u stride=%u)",
                                                     y[0], dst_w / 2, dst_h / 2, y[(dst_h / 2) * st + (dst_w / 2)],
                                                     dst_w, dst_h, st);
                                        munmap(p, seg_in->sizes[0]);
                                    }
                                }
                            }

                            // Segmentation infer + post
                            HalTensor sin{};
                            HalTensor sin2[2]{};
                            int snin = 1;
                            HalTensor sout[HAL_MAX_TENSORS]{};
                            int snum_out = 0;
                            uint32_t swant_in = 1;
                            {
                                HalModelInfo smi{};
                                if (HAL_INFERENCE_OPS.get_model_info(ctx->infer, &smi) == HAL_OK)
                                {
                                    snum_out = (int)((smi.num_outputs <= HAL_MAX_TENSORS) ? smi.num_outputs : HAL_MAX_TENSORS);
                                    swant_in = smi.num_inputs ? smi.num_inputs : 1U;
                                }
                            }
                            int sirc = HAL_ERR_NOT_SUPPORTED;
                            if (swant_in >= 2 && build_nv12_inputs_from_frame(seg_in, sin2, snin))
                                sirc = HAL_INFERENCE_OPS.run(ctx->infer, sin2, snin, sout, snum_out);
                            else if (HAL_INFERENCE_OPS.tensor_from_frame(seg_in, &sin) == HAL_OK)
                            {
                                sirc = HAL_INFERENCE_OPS.run(ctx->infer, &sin, 1, sout, snum_out);
                                HAL_INFERENCE_OPS.free_tensor(&sin);
                            }

                            HalPostprocessResult spr{};
                            if (sirc == HAL_OK && ctx->post && snum_out > 0 &&
                                HAL_POSTPROCESS_OPS.run(ctx->post, sout, snum_out, &spr) == HAL_OK &&
                                spr.type == HAL_POST_TYPE_SEGMENTATION && spr.result.segmentation.mask_data)
                            {
                                const auto &seg = spr.result.segmentation;
                                const uint32_t mw = seg.width;
                                const uint32_t mh = seg.height;
                                // Quick stats/debug.
                                for (uint32_t i = 0; i < mw * mh; i++)
                                {
                                    const uint8_t v = seg.mask_data[i];
                                    seg_min = std::min(seg_min, v);
                                    seg_max = std::max(seg_max, v);
                                    if (v != 0)
                                        seg_nonzero_total++;
                                }
                                // Aggregate: treat any non-zero class id as masked.
                                for (uint32_t y = 0; y < roi_h; y++)
                                {
                                    const uint32_t fy = roi.start_y + y;
                                    const uint32_t my = (y * mh) / roi_h;
                                    uint8_t *dst_row = &mask_u8[(size_t)fy * fw];
                                    for (uint32_t x = 0; x < roi_w; x++)
                                    {
                                        const uint32_t fx = roi.start_x + x;
                                        const uint32_t mx = (x * mw) / roi_w;
                                        const uint8_t cid = seg.mask_data[my * mw + mx];
                                    // linknet_dpm outputs either:
                                    // - class-id mask (0=bg, 1=fg), or
                                    // - uint8 "probability-like" mask (0..255 with fg near 255).
                                    // Mask only the likely-foreground values to avoid full-frame tint.
                                    const bool on = (cid == 1) || (cid >= 128);
                                    if (on)
                                        {
                                            dst_row[fx] = 255;
                                            masked_pixels_total++;
                                        }
                                    }
                                }

                                // Capture the per-ROI bytemask for the attach_frame_analytics path.
                                // Thresholded to 0/255; the blender scales this small mask to the ROI bbox.
                                DpmSegRoi sroi{};
                                sroi.x = static_cast<float>(roi.start_x) / static_cast<float>(fw);
                                sroi.y = static_cast<float>(roi.start_y) / static_cast<float>(fh);
                                sroi.w = static_cast<float>(roi_w) / static_cast<float>(fw);
                                sroi.h = static_cast<float>(roi_h) / static_cast<float>(fh);
                                sroi.mw = mw;
                                sroi.mh = mh;
                                sroi.mask.resize((size_t)mw * (size_t)mh);
                                for (size_t i = 0; i < sroi.mask.size(); i++)
                                {
                                    const uint8_t cid = seg.mask_data[i];
                                    sroi.mask[i] = ((cid == 1) || (cid >= 128)) ? 255 : 0;
                                }
                                std::snprintf(sroi.label, sizeof(sroi.label), "%s", dr.detections[di].label);
                                local_seg_rois.push_back(std::move(sroi));
                            }
                            HAL_POSTPROCESS_OPS.free_result(&spr);

                            for (int oi = 0; oi < snum_out; oi++)
                                if (sout[oi].data)
                                    HAL_INFERENCE_OPS.free_tensor(&sout[oi]);

                            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(seg_in);
                        }

                        if (ctx->verbose)
                        {
                            HAL_LOG_INFO("ai_example_v2: DPM seg stats dets=%u seg_nonzero=%llu masked_pixels=%llu seg_min=%u seg_max=%u",
                                         dr.num_detections,
                                         (unsigned long long)seg_nonzero_total,
                                         (unsigned long long)masked_pixels_total,
                                         (unsigned)seg_min,
                                         (unsigned)seg_max);
                        }

                        // Smooth mask (EMA on uint8).
                        {
                            const float a = std::clamp(ctx->dpm.smooth_alpha, 0.0f, 1.0f);
                            std::lock_guard<std::mutex> lk(ctx->dpm.mu);
                            ctx->dpm.last_w = fw;
                            ctx->dpm.last_h = fh;
                            ctx->dpm.last_rois = std::move(rois);
                            ctx->dpm.last_masked_pixels = masked_pixels_total;
                            ctx->dpm.seg_rois = std::move(local_seg_rois);
                            ctx->dpm.seg_rois_seq++;
                            if (!ctx->dpm.have_smoothed || ctx->dpm.smoothed_mask_u8.size() != mask_u8.size())
                            {
                                ctx->dpm.smoothed_mask_u8 = std::move(mask_u8);
                                ctx->dpm.have_smoothed = true;
                                ctx->dpm.mask_seq++;
                            }
                            else
                            {
                                for (size_t i = 0; i < mask_u8.size(); i++)
                                {
                                    const float v = a * (float)mask_u8[i] + (1.0f - a) * (float)ctx->dpm.smoothed_mask_u8[i];
                                    ctx->dpm.smoothed_mask_u8[i] = (uint8_t)std::lround(std::clamp(v, 0.0f, 255.0f));
                                }
                                ctx->dpm.mask_seq++;
                            }
                        }

                        // Publish detections for OSD boxes/labels.
                        publish_result(ctx, out);
                    }
                    HAL_POSTPROCESS_OPS.free_result(&dpr);

                    for (auto *b : det_chain)
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
                    if (det_in && det_in != job.frame)
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
                }

                sharedframe_unref(sf);
                continue;
            }

            // (1) Resize to model input if needed
            const HalFrameBuffer *ai_frame = job.frame;
            if (ctx->model_w && ctx->model_h &&
                (job.frame->width != ctx->model_w || job.frame->height != ctx->model_h))
            {
                HalFrameBufferRequest req{};
                req.width = ctx->model_w;
                req.height = ctx->model_h;
                req.format = HAL_PIX_FMT_NV12;
                req.mem_type = HAL_MEM_DMABUF;
                req.zero_initialize = false;
                if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &ai_resized) == HAL_OK && ai_resized)
                {
                    ml_clone_metadata_best_effort(job.frame, ai_resized);
                    if (dsp_resize_chain(ctx->dsp_ctx, job.frame, ai_resized, HAL_DSP_INTERPOLATION_BILINEAR, ai_resize_chain) == HAL_OK)
                        ai_frame = ai_resized;
                    else
                    {
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(ai_resized);
                        ai_resized = nullptr;
                    }
                }
            }

            // (2) Inference
            HalTensor in_t{};
            HalTensor in2[2]{};
            int nin = 1;
            HalTensor out_t[HAL_MAX_TENSORS]{};
            int num_out = 0;
            uint32_t want_in = 1;
            {
                HalModelInfo mi{};
                if (HAL_INFERENCE_OPS.get_model_info(ctx->infer, &mi) == HAL_OK)
                {
                    num_out = (int)((mi.num_outputs <= HAL_MAX_TENSORS) ? mi.num_outputs : HAL_MAX_TENSORS);
                    want_in = mi.num_inputs ? mi.num_inputs : 1U;
                }
            }

            const bool have_ai_input = (ai_frame && (!ctx->model_w || !ctx->model_h ||
                                                    (ai_frame->width == ctx->model_w && ai_frame->height == ctx->model_h)));
            if (have_ai_input)
            {
                int irc = HAL_ERR_NOT_SUPPORTED;
                const bool want_rgb =
                    (want_in == 1) &&
                    (ctx->preprocess_color_choice == PreprocessColorChoice::Nv12ToRgb ||
                     ctx->preprocess_color_choice == PreprocessColorChoice::Nv12ToBgr);

                HalFrameBuffer *rgb_fb = nullptr;
                if (want_rgb)
                {
                    // Ensure we feed packed RGB/BGR (HxWx3) for single-input models expecting 3 channels.
                    const HalFrameBuffer *src_nv12 = ai_frame;
                    if (ai_frame->format != HAL_PIX_FMT_NV12 || ai_frame->width != ctx->model_w || ai_frame->height != ctx->model_h)
                    {
                        // We already resized earlier into ai_resized when needed; prefer that if present.
                        // Fallback: require NV12 and correct size.
                    }
                    // If we have an NV12 resized buffer (ai_resized), use it; otherwise use ai_frame as-is.
                    // (ai_resized is in scope above; keep a safe check here.)
                    if (ai_resized && ai_resized->format == HAL_PIX_FMT_NV12 &&
                        ai_resized->width == ctx->model_w && ai_resized->height == ctx->model_h)
                    {
                        src_nv12 = ai_resized;
                    }

                    if (src_nv12 && src_nv12->format == HAL_PIX_FMT_NV12 &&
                        src_nv12->width == ctx->model_w && src_nv12->height == ctx->model_h)
                    {
                        HalFrameBufferRequest rreq{};
                        rreq.width = ctx->model_w;
                        rreq.height = ctx->model_h;
                        rreq.format = pixfmt_from_preprocess_choice(ctx->preprocess_color_choice);
                        rreq.mem_type = HAL_MEM_MALLOC;
                        rreq.zero_initialize = false;

                        if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&rreq, &rgb_fb) == HAL_OK && rgb_fb)
                        {
                            HalDspConvertFormatParams cp{};
                            cp.src = src_nv12;
                            cp.dst = rgb_fb;
                            if (HAL_DSP_OPS.convert_format(ctx->dsp_ctx, &cp) != HAL_OK)
                            {
                                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rgb_fb);
                                rgb_fb = nullptr;
                            }
                        }
                    }
                }

                if (rgb_fb)
                {
                    HalTensor rgb_in[1]{};
                    int rgb_nin = 1;
                    if (build_packed_rgb_inputs_from_frame(rgb_fb, rgb_in, rgb_nin))
                        irc = HAL_INFERENCE_OPS.run(ctx->infer, rgb_in, rgb_nin, out_t, num_out);
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(rgb_fb);
                    rgb_fb = nullptr;
                }
                else if (want_in >= 2 && build_nv12_inputs_from_frame(ai_frame, in2, nin))
                {
                    irc = HAL_INFERENCE_OPS.run(ctx->infer, in2, nin, out_t, num_out);
                }
                else if (HAL_INFERENCE_OPS.tensor_from_frame(ai_frame, &in_t) == HAL_OK)
                {
                    irc = HAL_INFERENCE_OPS.run(ctx->infer, &in_t, 1, out_t, num_out);
                    // tensor_from_frame allocates priv; free after run
                    HAL_INFERENCE_OPS.free_tensor(&in_t);
                }
                if (irc != HAL_OK)
                {
                    HAL_LOG_INFO("ai_example_v2: infer rc=%d", irc);
                }
                else
                {
                    g_ai_infer_ok.fetch_add(1, std::memory_order_relaxed);
                }
            }

        // (3) Postprocess + publish result (preview thread will draw)
            if (ctx->post && num_out > 0)
            {
                HalPostprocessResult pr{};
                const int prc = HAL_POSTPROCESS_OPS.run(ctx->post, out_t, num_out, &pr);
                if (prc == HAL_OK)
                {
                    g_ai_post_ok.fetch_add(1, std::memory_order_relaxed);
                    HalPostprocessResult pr_pub = synthesize_clip_from_embedding_if_enabled(ctx, pr);
                    // If a default/incorrect vendor classification plugin yields no classes,
                    // fall back to host-side top-k decoding from the raw output vector.
                    if (ctx->post_cfg.type == HAL_POST_TYPE_CLASSIFICATION &&
                        (pr_pub.type == HAL_POST_TYPE_CLASSIFICATION || pr_pub.type == HAL_POST_TYPE_CLIP) &&
                        pr_pub.result.classification.num_classes == 0)
                    {
                        HalPostprocessResult fb{};
                        if (synthesize_classification_from_raw_output(ctx, out_t, num_out, fb))
                            pr_pub = fb;
                    }
                    if (ctx->verbose && (pr_pub.type == HAL_POST_TYPE_CLIP || pr_pub.type == HAL_POST_TYPE_CLASSIFICATION))
                    {
                        const auto &cr = pr_pub.result.classification;
                        const uint32_t n = cr.num_classes;
                        if (n == 0)
                        {
                            HAL_LOG_INFO("ai_example_v2: clip classes=0");
                        }
                        else
                        {
                            std::string line = (pr_pub.type == HAL_POST_TYPE_CLASSIFICATION)
                                                   ? "ai_example_v2: classification topk:"
                                                   : "ai_example_v2: clip topk:";
                            const uint32_t k = (n > 5U) ? 5U : n;
                            for (uint32_t i = 0; i < k; i++)
                            {
                                char buf[256];
                                if (pr_pub.type == HAL_POST_TYPE_CLASSIFICATION)
                                {
                                    if (cr.classes[i].label[0] != '\0')
                                        std::snprintf(buf, sizeof(buf), " [%u]%s(%.3f)", i, cr.classes[i].label, cr.classes[i].confidence);
                                    else
                                        std::snprintf(buf, sizeof(buf), " [%u]id=%d(%.3f)", i, cr.classes[i].class_id, cr.classes[i].confidence);
                                }
                                else
                                    std::snprintf(buf, sizeof(buf), " [%u]%s(%.3f)", i, cr.classes[i].label, cr.classes[i].confidence);
                                line += buf;
                            }
                            HAL_LOG_INFO("%s", line.c_str());
                        }
                    }
                    else if (ctx->verbose && pr.type == HAL_POST_TYPE_EMBEDDING)
                    {
                        const auto &er = pr.result.embedding;
                        if (er.dim == 0)
                        {
                            HAL_LOG_INFO("ai_example_v2: embedding dim=0");
                        }
                        else
                        {
                            double sum = 0.0;
                            for (uint32_t i = 0; i < er.dim; i++)
                                sum += (double)er.data[i] * (double)er.data[i];
                            const double norm = std::sqrt(sum);
                            char first[256];
                            std::snprintf(first, sizeof(first),
                                          "ai_example_v2: embedding dim=%u norm=%.6f v0=%.5f v1=%.5f v2=%.5f v3=%.5f",
                                          (unsigned)er.dim, norm,
                                          er.data[0],
                                          (er.dim > 1 ? er.data[1] : 0.0f),
                                          (er.dim > 2 ? er.data[2] : 0.0f),
                                          (er.dim > 3 ? er.data[3] : 0.0f));
                            HAL_LOG_INFO("%s", first);
                        }
                    }
                    else if (ctx->verbose && pr.type == HAL_POST_TYPE_DEPTH)
                    {
                        const auto &dr = pr.result.depth;
                        if (!dr.depth_m || dr.width == 0u || dr.height == 0u)
                            HAL_LOG_INFO("ai_example_v2: depth map empty");
                        else
                        {
                            const size_t n = (size_t)dr.width * (size_t)dr.height;
                            float mn = dr.depth_m[0], mx = dr.depth_m[0];
                            for (size_t i = 1; i < n; i++)
                            {
                                mn = std::min(mn, dr.depth_m[i]);
                                mx = std::max(mx, dr.depth_m[i]);
                            }
                            HAL_LOG_INFO("ai_example_v2: depth %ux%u m in [%.3f, %.3f] (center=%.3f)", (unsigned)dr.width,
                                         (unsigned)dr.height, mn, mx, dr.depth_m[n / 2]);
                        }
                    }
                    publish_result(ctx, pr_pub);
                }
                HAL_POSTPROCESS_OPS.free_result(&pr);
            }
            else if (!ctx->post && ctx->post_cfg.type == HAL_POST_TYPE_CLASSIFICATION && num_out > 0)
            {
                // Fallback: decode a plain classification logits vector without a vendor postprocess plugin.
                HalPostprocessResult pr{};
                if (synthesize_classification_from_raw_output(ctx, out_t, num_out, pr))
                {
                    g_ai_post_ok.fetch_add(1, std::memory_order_relaxed);
                    publish_result(ctx, pr);

                    if (ctx->verbose)
                    {
                        const auto &cr = pr.result.classification;
                        if (cr.num_classes == 0)
                            HAL_LOG_INFO("ai_example_v2: classification fallback classes=0");
                        else
                        {
                            std::string line = "ai_example_v2: classification fallback topk:";
                            const uint32_t k = (cr.num_classes > 5U) ? 5U : cr.num_classes;
                            for (uint32_t i = 0; i < k; i++)
                            {
                                char buf[256];
                                if (cr.classes[i].label[0] != '\0')
                                    std::snprintf(buf, sizeof(buf), " [%u]%s(%.3f)", i, cr.classes[i].label, cr.classes[i].confidence);
                                else
                                    std::snprintf(buf, sizeof(buf), " [%u]id=%d(%.3f)", i, cr.classes[i].class_id, cr.classes[i].confidence);
                                line += buf;
                            }
                            HAL_LOG_INFO("%s", line.c_str());
                        }
                    }
                }
            }

            for (int i = 0; i < num_out; i++)
                if (out_t[i].data)
                    HAL_INFERENCE_OPS.free_tensor(&out_t[i]);
        }
        else
        {
            // Two-model landmarks mode: detector first.
            if (ctx->det_infer && ctx->det_post && ctx->det_w && ctx->det_h && ctx->model_w && ctx->model_h)
            {
                // A) Resize original -> detector input
                HalFrameBuffer *det_in = nullptr;
                std::vector<HalFrameBuffer *> det_chain;
                HalFrameBufferRequest dreq{};
                dreq.width = ctx->det_w;
                dreq.height = ctx->det_h;
                dreq.format = HAL_PIX_FMT_NV12;
                dreq.mem_type = HAL_MEM_DMABUF;
                dreq.zero_initialize = false;
                if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&dreq, &det_in) == HAL_OK && det_in)
                {
                    ml_clone_metadata_best_effort(job.frame, det_in);
                    if (dsp_resize_chain(ctx->dsp_ctx, job.frame, det_in, HAL_DSP_INTERPOLATION_BILINEAR, det_chain) != HAL_OK)
                    {
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
                        det_in = nullptr;
                    }
                }

                // B) Detector infer + post -> bbox
                HalDetection best_det{};
                bool have_det = false;
                if (det_in)
                {
                    HalTensor din{};
                    HalTensor din2[2]{};
                    int dnin = 1;
                    HalTensor dout[HAL_MAX_TENSORS]{};
                    int dnum_out = 0;
                    uint32_t dwant_in = 1;
                    {
                        HalModelInfo dmi{};
                        if (HAL_INFERENCE_OPS.get_model_info(ctx->det_infer, &dmi) == HAL_OK)
                        {
                            dnum_out = (int)((dmi.num_outputs <= HAL_MAX_TENSORS) ? dmi.num_outputs : HAL_MAX_TENSORS);
                            dwant_in = dmi.num_inputs ? dmi.num_inputs : 1U;
                        }
                    }
                    int dirc = HAL_ERR_NOT_SUPPORTED;
                    if (dwant_in >= 2 && build_nv12_inputs_from_frame(det_in, din2, dnin))
                    {
                        dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, din2, dnin, dout, dnum_out);
                    }
                    else if (HAL_INFERENCE_OPS.tensor_from_frame(det_in, &din) == HAL_OK)
                    {
                        dirc = HAL_INFERENCE_OPS.run(ctx->det_infer, &din, 1, dout, dnum_out);
                        HAL_INFERENCE_OPS.free_tensor(&din);
                    }
                    if (dirc != HAL_OK)
                        HAL_LOG_INFO("ai_example_v2: detector infer rc=%d", dirc);

                    HalPostprocessResult dpr{};
                    if (dnum_out > 0 && HAL_POSTPROCESS_OPS.run(ctx->det_post, dout, dnum_out, &dpr) == HAL_OK &&
                        dpr.type == HAL_POST_TYPE_DETECTION)
                    {
                        const auto &dr = dpr.result.detection;
                        float best_face = -1.0f;
                        bool have_face = false;
                        for (uint32_t i = 0; i < dr.num_detections; i++)
                        {
                            const HalDetection &dd = dr.detections[i];
                            const bool is_face = (std::strstr(dd.label, "face") != nullptr);
                            if (is_face && dd.confidence > best_face)
                            {
                                best_face = dd.confidence;
                                best_det = dd;
                                have_face = true;
                            }
                        }
                        have_det = have_face;
                        if (ctx->verbose)
                        {
                            if (!have_det)
                                HAL_LOG_INFO("ai_example_v2: skip landmarks (no face detection in detector results)");
                            else
                                HAL_LOG_INFO("ai_example_v2: selected face det label=%s conf=%.3f bbox=[%.3f,%.3f,%.3f,%.3f]",
                                             best_det.label, best_det.confidence,
                                             best_det.bbox.x, best_det.bbox.y, best_det.bbox.w, best_det.bbox.h);
                        }
                    }
                    HAL_POSTPROCESS_OPS.free_result(&dpr);

                    for (int i = 0; i < dnum_out; i++)
                        if (dout[i].data)
                            HAL_INFERENCE_OPS.free_tensor(&dout[i]);
                }

                // C) Crop original -> landmarks input (crop + resize)
                if (have_det)
                {
                    HalDspRoi roi{};
                    // V1-aligned: expand bbox a bit before cropping.
                    float cx = 0.0f, cy = 0.0f, cw = 0.0f, ch = 0.0f;
                    expand_face_bbox_norm(best_det.bbox.x, best_det.bbox.y, best_det.bbox.w, best_det.bbox.h,
                                          0.2f, cx, cy, cw, ch);
                    if (bbox_norm_to_roi(cx, cy, cw, ch, job.frame->width, job.frame->height, roi))
                    {
                        const uint32_t roi_w = roi.end_x - roi.start_x;
                        const uint32_t roi_h = roi.end_y - roi.start_y;

                        // First stage: crop_and_resize to a mid size that satisfies <=16x downscale from ROI.
                        constexpr uint32_t kMaxDownscale = 16U;
                        uint32_t mid_w = std::max(ctx->model_w, ceil_div_u32(roi_w, kMaxDownscale));
                        uint32_t mid_h = std::max(ctx->model_h, ceil_div_u32(roi_h, kMaxDownscale));
                        // NV12 requires even WxH for both src(after crop) and dst.
                        mid_w = align_up_even_u32(mid_w);
                        mid_h = align_up_even_u32(mid_h);
                        if (ctx->verbose)
                        {
                            HAL_LOG_INFO("ai_example_v2: face_roi=[%u,%u)->[%u,%u) roi=%ux%u mid=%ux%u lm=%ux%u",
                                         roi.start_x, roi.start_y, roi.end_x, roi.end_y,
                                         roi_w, roi_h, mid_w, mid_h, ctx->model_w, ctx->model_h);
                        }

                        HalFrameBuffer *mid = nullptr;
                        HalFrameBufferRequest mreq{};
                        mreq.width = mid_w;
                        mreq.height = mid_h;
                        mreq.format = HAL_PIX_FMT_NV12;
                        mreq.mem_type = HAL_MEM_DMABUF;
                        mreq.zero_initialize = false;
                        if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&mreq, &mid) == HAL_OK && mid)
                        {
                            ml_clone_metadata_best_effort(job.frame, mid);
                            HalDspCropResizeParams cp{};
                            cp.src = job.frame;
                            cp.dst = mid;
                            cp.crop = roi;
                            cp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
                            cp.scaling_mode = HAL_DSP_SCALING_STRETCH;
                        cp.letterbox_alignment = HAL_DSP_LETTERBOX_MIDDLE;
                        cp.letterbox_color = HalDspColor{.y = 0, .u = 128, .v = 128};

                            if (HAL_DSP_OPS.crop_and_resize(ctx->dsp_ctx, &cp) == HAL_OK)
                            {
                                HalFrameBuffer *lm_in = nullptr;
                                std::vector<HalFrameBuffer *> lm_chain;
                                if (mid_w == ctx->model_w && mid_h == ctx->model_h)
                                {
                                    lm_in = mid;
                                }
                                else
                                {
                                    HalFrameBufferRequest lreq{};
                                    lreq.width = ctx->model_w;
                                    lreq.height = ctx->model_h;
                                    lreq.format = HAL_PIX_FMT_NV12;
                                    lreq.mem_type = HAL_MEM_DMABUF;
                                    lreq.zero_initialize = false;
                                    if (HAL_FRAME_BUFFER_OPS.request_frame_buffer(&lreq, &lm_in) == HAL_OK && lm_in)
                                    {
                                        ml_clone_metadata_best_effort(job.frame, lm_in);
                                        if (dsp_resize_chain(ctx->dsp_ctx, mid, lm_in, HAL_DSP_INTERPOLATION_BILINEAR, lm_chain) != HAL_OK)
                                        {
                                            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(lm_in);
                                            lm_in = nullptr;
                                        }
                                    }
                                }

                                if (lm_in)
                                {
                                    // D) Landmarks inference + post + draw
                                    HalTensor lin{};
                                    HalTensor lin2[2]{};
                                    int lnin = 1;
                                    HalTensor lout[HAL_MAX_TENSORS]{};
                                    int lnum_out = 0;
                                    uint32_t lwant_in = 1;
                                    {
                                        HalModelInfo lmi{};
                                        if (HAL_INFERENCE_OPS.get_model_info(ctx->infer, &lmi) == HAL_OK)
                                        {
                                            lnum_out = (int)((lmi.num_outputs <= HAL_MAX_TENSORS) ? lmi.num_outputs : HAL_MAX_TENSORS);
                                            lwant_in = lmi.num_inputs ? lmi.num_inputs : 1U;
                                        }
                                    }
                                    int lirc = HAL_ERR_NOT_SUPPORTED;
                                    if (lwant_in >= 2 && build_nv12_inputs_from_frame(lm_in, lin2, lnin))
                                    {
                                        lirc = HAL_INFERENCE_OPS.run(ctx->infer, lin2, lnin, lout, lnum_out);
                                    }
                                    else if (HAL_INFERENCE_OPS.tensor_from_frame(lm_in, &lin) == HAL_OK)
                                    {
                                        lirc = HAL_INFERENCE_OPS.run(ctx->infer, &lin, 1, lout, lnum_out);
                                        HAL_INFERENCE_OPS.free_tensor(&lin);
                                    }
                                    if (lirc != HAL_OK)
                                        HAL_LOG_INFO("ai_example_v2: landmarks infer rc=%d", lirc);

                                    if (ctx->post && lnum_out > 0)
                                    {
                                        HalPostprocessResult pr{};
                                        const int prc = HAL_POSTPROCESS_OPS.run(ctx->post, lout, lnum_out, &pr);
                                        if (prc == HAL_OK)
                                        {
                                            if (ctx->verbose && pr.type == HAL_POST_TYPE_KEYPOINT)
                                            {
                                                HAL_LOG_INFO("ai_example_v2: landmarks post OK objects=%u kp0=%u",
                                                             pr.result.keypoint.num_objects,
                                                             (pr.result.keypoint.num_objects > 0) ? pr.result.keypoint.objects[0].num_keypoints : 0U);
                                            }
                                            // Keypoints are produced in crop space; remap back to full-frame coordinates.
                                            remap_keypoints_to_frame(pr, roi, job.frame->width, job.frame->height, ctx->model_w, ctx->model_h);
                                            publish_result(ctx, pr);
                                        }
                                        else if (ctx->verbose)
                                        {
                                            HAL_LOG_INFO("ai_example_v2: landmarks post rc=%d", prc);
                                        }
                                        HAL_POSTPROCESS_OPS.free_result(&pr);
                                    }

                                    for (int i = 0; i < lnum_out; i++)
                                        if (lout[i].data)
                                            HAL_INFERENCE_OPS.free_tensor(&lout[i]);
                                }

                                for (auto *b : lm_chain)
                                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
                                if (lm_in && lm_in != mid)
                                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(lm_in);
                            }

                            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(mid);
                        }
                    }
                }

                // cleanup detector resize buffers
                for (auto *b : det_chain)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);
                if (det_in)
                    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(det_in);
            }
        }

        if (ai_resized)
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(ai_resized);
        for (auto *b : ai_resize_chain)
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(b);

        sharedframe_unref(sf);
    }
}

static void preview_worker_loop(AppCtx *ctx)
{
    if (!ctx || !ctx->codec_ctx)
        return;
    uint64_t last_seq = 0;
    const auto hold_ms = std::chrono::milliseconds(200);
    while (!g_stop.load(std::memory_order_acquire))
    {
        SharedFrame *sf = nullptr;
        {
            std::unique_lock<std::mutex> lk(ctx->preview_mu);
            ctx->preview_cv.wait(lk, [&] { return g_stop.load(std::memory_order_acquire) || !ctx->preview_q.empty(); });
            if (g_stop.load(std::memory_order_acquire))
                break;
            sf = ctx->preview_q.front();
            ctx->preview_q.pop_front();
        }
        if (!sf)
            continue;
        FrameJob job{};
        job.video_ctx = sf->video_ctx;
        job.frame = &sf->fb;

        HalPostprocessResult pr{};
        bool have_pr = false;
        bool use_hold = false;
        {
            std::unique_lock<std::mutex> lk(ctx->result.mu);
            ctx->result.cv.wait_for(lk, std::chrono::milliseconds(10), [&] {
                return g_stop.load(std::memory_order_acquire) || (ctx->result.valid && ctx->result.seq != last_seq);
            });
            if (ctx->result.valid && ctx->result.seq != last_seq)
            {
                pr = ctx->result.last;
                last_seq = ctx->result.seq;
                have_pr = true;
            }
            else if (ctx->result.has_valid)
            {
                auto now = std::chrono::steady_clock::now();
                if (now - ctx->result.last_valid_tp <= hold_ms)
                {
                    pr = ctx->result.last_valid;
                    use_hold = true;
                }
            }
        }
        if (have_pr || use_hold)
        {
            // DPM render (privacy mask) on the preview frame before drawing OSD.
            if (ctx->dpm.enabled)
            {
                std::lock_guard<std::mutex> lk(ctx->dpm.mu);
                const uint32_t mw = ctx->dpm.last_w;
                const uint32_t mh = ctx->dpm.last_h;
                const uint32_t fw = job.frame->width;
                const uint32_t fh = job.frame->height;
                const bool dims_match = (mw == fw && mh == fh);
                const bool mask_ok = (ctx->dpm.have_smoothed && mw > 0 && mh > 0 &&
                                      ctx->dpm.smoothed_mask_u8.size() == (size_t)mw * (size_t)mh);

                if (ctx->dpm.render_mode == DpmRenderMode::Overlay)
                {
                    if (mask_ok)
                    {
                        const uint8_t *mask_ptr = nullptr;
                        if (dims_match)
                        {
                            mask_ptr = ctx->dpm.smoothed_mask_u8.data();
                        }
                        else
                        {
                            if (ctx->dpm.scaled_w != fw || ctx->dpm.scaled_h != fh ||
                                ctx->dpm.scaled_mask_u8.size() != (size_t)fw * (size_t)fh)
                            {
                                ctx->dpm.scaled_w = fw;
                                ctx->dpm.scaled_h = fh;
                                ctx->dpm.scaled_mask_u8.assign((size_t)fw * (size_t)fh, 0);
                                ctx->dpm.scaled_from_seq = 0;
                            }
                            // Only rescale when a new mask arrives (or when preview dims change).
                            if (ctx->dpm.scaled_from_seq != ctx->dpm.mask_seq)
                            {
                                scale_mask_u8_nn(ctx->dpm.smoothed_mask_u8.data(), mw, mh,
                                                 ctx->dpm.scaled_mask_u8.data(), fw, fh);
                                ctx->dpm.scaled_from_seq = ctx->dpm.mask_seq;
                            }
                            mask_ptr = ctx->dpm.scaled_mask_u8.data();

                            // Throttle dims mismatch log (roughly 1/120 frames) to avoid spam.
                            ctx->dpm.scale_log_throttle++;
                            if ((ctx->dpm.scale_log_throttle % 120) == 0)
                                HAL_LOG_INFO("ai_example_v2: DPM overlay scale mask %ux%u -> %ux%u (seq=%llu)",
                                             mw, mh, fw, fh, (unsigned long long)ctx->dpm.mask_seq);
                        }

                        if (ctx->dpm.last_masked_pixels > 0)
                        {
                            // Throttle to avoid spamming logs (roughly 1/60 frames).
                            ctx->dpm.overlay_log_throttle++;
                            if ((ctx->dpm.overlay_log_throttle % 60) == 0)
                                HAL_LOG_INFO("ai_example_v2: DPM overlay draw masked_pixels=%llu",
                                             (unsigned long long)ctx->dpm.last_masked_pixels);
                        }
                        HalDrawMask m{};
                        m.x = 0;
                        m.y = 0;
                        m.width = fw;
                        m.height = fh;
                        // Apply a small threshold to avoid "ghost" tint from EMA tail.
                        // (cpu_draw_mask treats any non-zero as masked).
                        if (!dims_match)
                        {
                            for (size_t i = 0; i < ctx->dpm.scaled_mask_u8.size(); i++)
                                if (ctx->dpm.scaled_mask_u8[i] < 16)
                                    ctx->dpm.scaled_mask_u8[i] = 0;
                            m.mask_data = ctx->dpm.scaled_mask_u8.data();
                        }
                        else
                        {
                            m.mask_data = (uint8_t *)mask_ptr;
                        }
                        m.color = hal_color_rgb(0, 255, 255);
                        m.alpha = 0.45f;
                        (void)HAL_DRAW_OPS.draw_mask(job.frame, &m);
                    }
                }
                else
                {
                    // Prefer hardware privacy masking for Mosaic/Blur modes (aligned with official pipeline):
                    // Use DSP privacy mask driven by segmentation bitmask (shape follows the mask, not just rectangles).
                    // --dpm-attach takes this path too (metadata attached in dpm_apply_attach_privacy_mask).
                    if (ctx->dpm.hw_privacy_mask || ctx->dpm.use_attach_api)
                    {
                        // Cached bitmask/metadata is refreshed inside dpm_apply_*_privacy_mask when needed.
                    }
                    else
                    {
                        // Software fallback: Mosaic/blur based on the segmentation mask (not the whole ROI).
                        if (mask_ok && dims_match)
                        {
                            const uint8_t *mask = ctx->dpm.smoothed_mask_u8.data();
                            const int bs = (ctx->dpm.render_mode == DpmRenderMode::Blur) ? 0 : (int)ctx->dpm.mosaic_block_size;
                            const int step = (bs > 0) ? std::max(2, bs) : 8; // for blur we still process small tiles

                            for (const auto &r : ctx->dpm.last_rois)
                            {
                                const int x0 = std::max(0, r.x);
                                const int y0 = std::max(0, r.y);
                                const int x1 = std::min((int)fw, r.x + r.width);
                                const int y1 = std::min((int)fh, r.y + r.height);
                                if (x0 >= x1 || y0 >= y1)
                                    continue;

                                for (int by = y0; by < y1; by += step)
                                {
                                    for (int bx = x0; bx < x1; bx += step)
                                    {
                                        const int ex = std::min(bx + step, x1);
                                        const int ey = std::min(by + step, y1);
                                        bool any = false;
                                        for (int yy = by; yy < ey && !any; yy++)
                                        {
                                            const uint8_t *row = &mask[(size_t)yy * (size_t)fw];
                                            for (int xx = bx; xx < ex; xx++)
                                            {
                                                if (row[xx] != 0)
                                                {
                                                    any = true;
                                                    break;
                                                }
                                            }
                                        }
                                        if (!any)
                                            continue;

                                        HalDrawMosaic mm{};
                                        mm.x = bx;
                                        mm.y = by;
                                        mm.width = ex - bx;
                                        mm.height = ey - by;
                                        mm.block_size = bs;
                                        (void)HAL_DRAW_OPS.draw_mosaic(job.frame, &mm);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            if (ctx->dpm.enabled)
            {
                if (ctx->dpm.use_attach_api)
                    dpm_apply_attach_privacy_mask(ctx, job.frame);
                else if (ctx->dpm.hw_privacy_mask)
                    dpm_apply_hw_privacy_mask(ctx, job.frame);
            }

            if (pr.type == HAL_POST_TYPE_CLIP && pr.result.classification.num_classes > 0)
            {
                auto &cl = pr.result.classification;
                constexpr float a = 0.12f;
                const uint32_t n = std::min(cl.num_classes, AppCtx::kClipOsdEmaSlots);
                if (ctx->clip_osd_ema_num_classes != cl.num_classes)
                {
                    ctx->clip_osd_ema_num_classes = cl.num_classes;
                    for (uint32_t i = 0; i < n; i++)
                        ctx->clip_osd_ema[i] = cl.classes[i].confidence;
                }
                else
                {
                    for (uint32_t i = 0; i < n; i++)
                        ctx->clip_osd_ema[i] =
                            a * cl.classes[i].confidence + (1.0f - a) * ctx->clip_osd_ema[i];
                }
                for (uint32_t i = 0; i < n; i++)
                    cl.classes[i].confidence = ctx->clip_osd_ema[i];
            }
            else if (pr.type != HAL_POST_TYPE_CLIP)
                ctx->clip_osd_ema_num_classes = 0;

            (void)HAL_DRAW_OPS.draw_result(&pr, job.frame, &ctx->draw_cfg);
        }

        (void)HAL_CODEC_OPS.input_frame(ctx->codec_ctx, job.frame);
        g_frames_encoded_in.fetch_add(1, std::memory_order_relaxed);

        sharedframe_unref(sf);
    }
}

static void scale_mask_u8_nn(const uint8_t *src, uint32_t sw, uint32_t sh,
                             uint8_t *dst, uint32_t dw, uint32_t dh)
{
    if (!src || !dst || sw == 0 || sh == 0 || dw == 0 || dh == 0)
        return;
    for (uint32_t y = 0; y < dh; y++)
    {
        const uint32_t sy = (uint32_t)(((uint64_t)y * (uint64_t)sh) / (uint64_t)dh);
        const uint8_t *srow = src + (size_t)sy * (size_t)sw;
        uint8_t *drow = dst + (size_t)y * (size_t)dw;
        for (uint32_t x = 0; x < dw; x++)
        {
            const uint32_t sx = (uint32_t)(((uint64_t)x * (uint64_t)sw) / (uint64_t)dw);
            drow[x] = srow[sx];
        }
    }
}

static uint32_t align_up_u32(uint32_t v, uint32_t a)
{
    if (a == 0) return v;
    const uint32_t r = v % a;
    return r ? (v + (a - r)) : v;
}

static void dpm_build_dsp_bitmask_and_rois_locked(AppCtx *ctx, uint32_t frame_w, uint32_t frame_h)
{
    if (!ctx || frame_w == 0 || frame_h == 0)
        return;
    if (ctx->dpm.scaled_mask_u8.size() != (size_t)frame_w * (size_t)frame_h)
        return;

    // Bitmask is 4x4-quantized: one bit represents a 4x4 block in the original image.
    const uint32_t bw = (frame_w + 3U) / 4U;
    const uint32_t bh = (frame_h + 3U) / 4U;
    const uint32_t row_bytes = (bw + 7U) / 8U;
    const uint32_t stride = align_up_u32(row_bytes, 8U); // DSP requires stride divisible by 8

    ctx->dpm.bm_w = bw;
    ctx->dpm.bm_h = bh;
    ctx->dpm.bm_stride = stride;
    ctx->dpm.bm_bits.assign((size_t)stride * (size_t)bh, 0);

    const uint8_t *mask = ctx->dpm.scaled_mask_u8.data();
    bool any_bit = false;
    uint32_t min_bx = bw, min_by = bh, max_bx = 0, max_by = 0; // bbox in bitmask coords (inclusive)
    for (uint32_t by = 0; by < bh; by++)
    {
        for (uint32_t bx = 0; bx < bw; bx++)
        {
            bool any = false;
            const uint32_t x0 = bx * 4U;
            const uint32_t y0 = by * 4U;
            const uint32_t x1 = std::min(frame_w, x0 + 4U);
            const uint32_t y1 = std::min(frame_h, y0 + 4U);
            for (uint32_t yy = y0; yy < y1 && !any; yy++)
            {
                const uint8_t *row = &mask[(size_t)yy * (size_t)frame_w];
                for (uint32_t xx = x0; xx < x1; xx++)
                {
                    if (row[xx] != 0)
                    {
                        any = true;
                        break;
                    }
                }
            }
            if (!any)
                continue;
            const uint32_t byte_i = bx / 8U;
            const uint32_t bit_i = bx % 8U;
            ctx->dpm.bm_bits[(size_t)by * (size_t)stride + (size_t)byte_i] |= (uint8_t)(1U << bit_i);
            any_bit = true;
            min_bx = std::min(min_bx, bx);
            min_by = std::min(min_by, by);
            max_bx = std::max(max_bx, bx);
            max_by = std::max(max_by, by);
        }
    }

    // ROI hints in bitmask space (4x4 quantized image). Limit to 8 ROIs.
    ctx->dpm.bm_rois.clear();
    ctx->dpm.bm_rois.reserve(8);

    if (!any_bit)
    {
        // Nothing to mask; leave rois empty so caller can skip DSP call.
        return;
    }

    const uint32_t src_w = ctx->dpm.last_w ? ctx->dpm.last_w : frame_w;
    const uint32_t src_h = ctx->dpm.last_h ? ctx->dpm.last_h : frame_h;
    for (size_t i = 0; i < ctx->dpm.last_rois.size() && ctx->dpm.bm_rois.size() < 8; i++)
    {
        const auto &r = ctx->dpm.last_rois[i];
        const int sx0 = std::max(0, r.x);
        const int sy0 = std::max(0, r.y);
        const int sx1 = std::min((int)src_w, r.x + r.width);
        const int sy1 = std::min((int)src_h, r.y + r.height);
        if (sx0 >= sx1 || sy0 >= sy1)
            continue;

        const int x0 = (int)(((int64_t)sx0 * (int64_t)frame_w) / (int64_t)src_w);
        const int y0 = (int)(((int64_t)sy0 * (int64_t)frame_h) / (int64_t)src_h);
        const int x1 = (int)(((int64_t)sx1 * (int64_t)frame_w) / (int64_t)src_w);
        const int y1 = (int)(((int64_t)sy1 * (int64_t)frame_h) / (int64_t)src_h);

        HalDspRoi roi{};
        roi.start_x = (uint32_t)std::clamp(x0 / 4, 0, (int)bw);
        roi.start_y = (uint32_t)std::clamp(y0 / 4, 0, (int)bh);
        roi.end_x = (uint32_t)std::clamp((x1 + 3) / 4, 0, (int)bw);
        roi.end_y = (uint32_t)std::clamp((y1 + 3) / 4, 0, (int)bh);
        if (roi.start_x >= roi.end_x || roi.start_y >= roi.end_y)
            continue;
        ctx->dpm.bm_rois.push_back(roi);
    }
    if (ctx->dpm.bm_rois.empty())
    {
        // Fallback: bounding ROI from the mask bitmask itself (much cheaper than full-frame ROI).
        HalDspRoi roi{};
        roi.start_x = min_bx;
        roi.start_y = min_by;
        roi.end_x = std::min(bw, max_bx + 1U);
        roi.end_y = std::min(bh, max_by + 1U);
        if (roi.start_x < roi.end_x && roi.start_y < roi.end_y)
            ctx->dpm.bm_rois.push_back(roi);
    }
}

static void dpm_apply_hw_privacy_mask(AppCtx *ctx, HalFrameBuffer *frame)
{
    if (!ctx || !ctx->dsp_ctx || !frame || frame->width == 0 || frame->height == 0)
        return;

    HalDspPrivacyMaskRegion region{};
    HalDspPrivacyMaskParams pm{};
    pm.image = frame;
    pm.regions = &region;
    pm.region_count = 1;

    {
        std::lock_guard<std::mutex> lk(ctx->dpm.mu);
        // Re-check under lock to avoid duplicate work.
        if (!ctx->dpm.have_smoothed || ctx->dpm.smoothed_mask_u8.empty() || ctx->dpm.last_w == 0 || ctx->dpm.last_h == 0)
            return;

        // Critical fast-path: if nothing is masked, do not spend CPU on scaling/bitmask generation.
        if (ctx->dpm.last_masked_pixels == 0)
        {
            ctx->dpm.bm_rois.clear();
            ctx->dpm.bm_bits.clear();
            return;
        }

        // Ensure we have a scaled mask in preview/encoder resolution.
        const uint32_t frame_w = frame->width;
        const uint32_t frame_h = frame->height;
        if (ctx->dpm.scaled_w != frame_w || ctx->dpm.scaled_h != frame_h ||
            ctx->dpm.scaled_mask_u8.size() != (size_t)frame_w * (size_t)frame_h)
        {
            ctx->dpm.scaled_w = frame_w;
            ctx->dpm.scaled_h = frame_h;
            ctx->dpm.scaled_mask_u8.assign((size_t)frame_w * (size_t)frame_h, 0);
            ctx->dpm.scaled_from_seq = 0;
        }
        if (ctx->dpm.scaled_from_seq != ctx->dpm.mask_seq)
        {
            scale_mask_u8_nn(ctx->dpm.smoothed_mask_u8.data(), ctx->dpm.last_w, ctx->dpm.last_h,
                             ctx->dpm.scaled_mask_u8.data(), frame_w, frame_h);
            // Kill EMA tail to avoid full-frame masking.
            for (size_t i = 0; i < ctx->dpm.scaled_mask_u8.size(); i++)
                if (ctx->dpm.scaled_mask_u8[i] < 16)
                    ctx->dpm.scaled_mask_u8[i] = 0;
            ctx->dpm.scaled_from_seq = ctx->dpm.mask_seq;
        }

        // Rebuild bitmask/rois only when new DPM result arrives or when dims changed.
        if (ctx->dpm.hw_applied_seq != ctx->dpm.mask_seq ||
            ctx->dpm.bm_w != ((frame_w + 3U) / 4U) || ctx->dpm.bm_h != ((frame_h + 3U) / 4U))
        {
            dpm_build_dsp_bitmask_and_rois_locked(ctx, frame_w, frame_h);
            ctx->dpm.hw_applied_seq = ctx->dpm.mask_seq;
        }

        if (ctx->dpm.bm_rois.empty() || ctx->dpm.bm_bits.empty())
        {
            // Nothing to mask on this frame.
            return;
        }

        region.bitmask = ctx->dpm.bm_bits.data();
        region.stride_bytes = ctx->dpm.bm_stride;
        region.type = HAL_DSP_PRIVACY_MASK_BLUR;
        region.blur_radius = (ctx->dpm.render_mode == DpmRenderMode::Blur) ? 24U : (uint32_t)std::max(2U, ctx->dpm.mosaic_block_size);
        if (region.blur_radius > 64U) region.blur_radius = 64U;
        if (region.blur_radius % 2U) region.blur_radius--;
        if (region.blur_radius < 2U) region.blur_radius = 2U;
        region.rois = ctx->dpm.bm_rois.data();
        region.roi_count = (uint32_t)ctx->dpm.bm_rois.size();

        ctx->dpm.hw_log_throttle++;
        if ((ctx->dpm.hw_log_throttle % 60) == 0)
            HAL_LOG_INFO("ai_example_v2: DPM DSP privacy_mask apply bitmask=%ux%u stride=%u rois=%u radius=%u seq=%llu",
                         ctx->dpm.bm_w, ctx->dpm.bm_h, ctx->dpm.bm_stride, region.roi_count,
                         (unsigned)region.blur_radius, (unsigned long long)ctx->dpm.mask_seq);
    }

    // Apply on the current frame in DSP (in-place).
    (void)HAL_DSP_OPS.privacy_mask(ctx->dsp_ctx, &pm);
}

// Attach per-ROI segmentation masks to the frame's m_analytics_metadata so the media library DSP
// privacy-mask blender (dynamic path) draws them during encode. Each detection ROI's small bytemask
// is passed as one HalFrameSegmentation; the blender scales it to the ROI bbox (irregular shape,
// not a solid block). This matches the blender's design — do NOT pass a full-frame mask.
static void dpm_apply_attach_privacy_mask(AppCtx *ctx, HalFrameBuffer *frame)
{
    if (!ctx || !frame || !HAL_MEDIA_OPS.attach_frame_analytics || !ctx->media_ctx)
        return;

    // Snapshot the latest per-ROI masks (small; copy is cheap). They stay valid for the attach call
    // (attach_frame_analytics copies the bytemask into HAL-owned storage pinned for the DSP blend).
    std::vector<DpmSegRoi> snap;
    {
        std::lock_guard<std::mutex> lk(ctx->dpm.mu);
        snap = ctx->dpm.seg_rois;
    }

    if (snap.empty())
    {
        // No detections this DPM cycle — clear stale metadata so the blender draws nothing.
        (void)HAL_MEDIA_OPS.attach_frame_analytics(ctx->media_ctx, frame, nullptr, 0, nullptr, 0);
        return;
    }

    // The blender's seg path assumes the mask is letterboxed (ROI centered, 0 in padding) and
    // applies letterbox-unwrapping math. The seg model can over-segment into the letterbox bars,
    // which the blender then maps outside the ROI (blur stretches). Clamp: recompute the letterbox
    // content region from the ROI aspect and zero everything outside it, so the mask matches the
    // blender's contract. ROI pixel aspect is taken from the frame the seg ran on.
    const uint32_t fw = frame->width;
    const uint32_t fh = frame->height;
    std::vector<HalFrameSegmentation> segs;
    std::vector<std::vector<uint8_t>> clamped_storage; // keep masks alive across the attach call
    segs.reserve(snap.size());
    clamped_storage.reserve(snap.size());
    for (size_t i = 0; i < snap.size(); i++)
    {
        const auto &sr = snap[i];
        if (sr.mw == 0 || sr.mh == 0 || sr.mask.empty())
            continue;

        // ROI pixel dims → letterbox content region inside the mw×mh mask.
        const float roi_w_px = sr.w * (float)fw;
        const float roi_h_px = sr.h * (float)fh;
        if (roi_w_px < 1.0f || roi_h_px < 1.0f)
            continue;
        const float scale = std::min((float)sr.mw / roi_w_px, (float)sr.mh / roi_h_px);
        const uint32_t content_w = std::max(1u, (uint32_t)std::lround(roi_w_px * scale));
        const uint32_t content_h = std::max(1u, (uint32_t)std::lround(roi_h_px * scale));
        const uint32_t pad_x = (sr.mw - content_w) / 2;
        const uint32_t pad_y = (sr.mh - content_h) / 2;

        // The seg model is stretch-trained: it outputs the person filling the full 256x256 (stretched
        // to square), not letterboxed. The blender's seg path needs a letterboxed mask (content in
        // the ROI's aspect, centered, 0 in bars). Resample the stretched mask into the letterbox
        // content region (un-stretch to ROI aspect), zero the bars. recttest: fill content solid.
        clamped_storage.emplace_back(sr.mask.size(), 0);
        std::vector<uint8_t> &out = clamped_storage.back();
        const bool recttest = ctx->dpm.recttest;
        for (uint32_t cy = 0; cy < content_h; cy++)
        {
            const uint32_t src_y = (content_h == sr.mh) ? cy : (uint32_t)((uint64_t)cy * sr.mh / content_h);
            const uint8_t *src_row = &sr.mask[(size_t)src_y * sr.mw];
            uint8_t *dst_row = &out[(size_t)(pad_y + cy) * sr.mw + pad_x];
            if (recttest)
            {
                std::memset(dst_row, 255, content_w);
                continue;
            }
            for (uint32_t cx = 0; cx < content_w; cx++)
            {
                const uint32_t src_x = (content_w == sr.mw) ? cx : (uint32_t)((uint64_t)cx * sr.mw / content_w);
                dst_row[cx] = src_row[src_x];
            }
        }

        HalFrameSegmentation s{};
        std::snprintf(s.label, sizeof(s.label), "%s", sr.label);
        s.class_id = 0;
        s.x = sr.x;
        s.y = sr.y;
        s.w = sr.w;
        s.h = sr.h;
        s.mask_w = sr.mw;
        s.mask_h = sr.mh;
        s.mask = out.data();
        segs.push_back(s);
    }

    if (segs.empty())
    {
        (void)HAL_MEDIA_OPS.attach_frame_analytics(ctx->media_ctx, frame, nullptr, 0, nullptr, 0);
        return;
    }

    int rc = HAL_MEDIA_OPS.attach_frame_analytics(ctx->media_ctx, frame, nullptr, 0, segs.data(),
                                                  static_cast<uint32_t>(segs.size()));
    {
        static uint32_t throttle = 0;
        if (rc != HAL_OK && (throttle++ % 60) == 0)
            HAL_LOG_INFO("ai_example_v2: attach_frame_analytics(seg) failed rc=%d segs=%zu", rc, segs.size());
        else if (rc == HAL_OK && (throttle++ % 60) == 0)
        {
            const auto &s0 = segs[0];
            // Content region (post-clamp) for diag.
            const float sc = std::min((float)s0.mask_w / (s0.w * (float)fw), (float)s0.mask_h / (s0.h * (float)fh));
            HAL_LOG_INFO("ai_example_v2: attach(seg) ok segs=%zu bbox=[%.3f,%.3f,%.3f,%.3f] mask=%ux%u content=%ux%u pad=%u,%u",
                         segs.size(), s0.x, s0.y, s0.w, s0.h, s0.mask_w, s0.mask_h,
                         (uint32_t)std::lround(s0.w * (float)fw * sc), (uint32_t)std::lround(s0.h * (float)fh * sc),
                         (s0.mask_w - (uint32_t)std::lround(s0.w * (float)fw * sc)) / 2,
                         (s0.mask_h - (uint32_t)std::lround(s0.h * (float)fh * sc)) / 2);
        }
    }
}

static bool parse_host_port(const std::string &s, std::string &host, uint16_t &port)
{
    auto pos = s.find(':');
    if (pos == std::string::npos) return false;
    host = s.substr(0, pos);
    port = (uint16_t)std::atoi(s.substr(pos + 1).c_str());
    return !host.empty() && port != 0;
}

static bool is_number(const std::string &s)
{
    if (s.empty()) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c))
            return false;
    return true;
}

static std::vector<std::string> split_csv_trim(const std::string &csv)
{
    std::vector<std::string> out;
    size_t i = 0;
    while (i < csv.size())
    {
        size_t j = csv.find(',', i);
        if (j == std::string::npos)
            j = csv.size();
        size_t a = i;
        size_t b = j;
        while (a < b && std::isspace((unsigned char)csv[a]))
            a++;
        while (b > a && std::isspace((unsigned char)csv[b - 1]))
            b--;
        if (b > a)
            out.emplace_back(csv.substr(a, b - a));
        i = j + 1;
    }
    return out;
}

} // namespace

int main(int argc, char **argv)
{
    std::string media_json;
    std::string profile_arg;
    std::string model_path;
    std::string detector_model_path = "/home/root/apps/face_landmarks/resources/hailo_yolov8n_384_640.hef";
    std::string det_post_cfg_file = "/home/root/apps/webserver/resources/configs/yolov5_personface.json";
    std::string det_post_cfg_json;
    std::string udp_arg = "127.0.0.1:5004";
    std::string post_type = "auto"; // auto|detection|classification|clip|segmentation|keypoint|embedding|depth|ocr|none
    std::string post_cfg_json;
    std::string post_cfg_file;
    std::string preprocess_color = "auto"; // auto|none|nv12_to_rgb|nv12_to_bgr
    /** ImageNet `imagenet_class_index.json` (Torchvision-style) for classification label strings. */
    std::string classification_labels_json;
    bool verbose = false;

    bool dpm = false;
    std::string dpm_mode = "mosaic"; // mosaic|blur|overlay
    std::string dpm_labels = "person,vehicle";
    uint32_t dpm_max_rois = 35;
    float dpm_smooth_alpha = 0.5f;
    uint32_t dpm_mask_size = 128;
    uint32_t dpm_block_size = 24;
    bool dpm_attach = false; // --dpm-attach: render via attach_frame_analytics (media blender dynamic path)
    bool dpm_recttest = false; // --dpm-attach-recttest: solid content rect to isolate DSP placement

    // Optional: on-device CLIP text encoder resources (to score image embeddings against prompts).
    /** Use HalClipTextEncoder::init() with HalClipTextEncoderConfig::default_config() (paths under /home/root/apps/clip/resources/). */
    bool clip_text_default = false;
    std::string clip_text_hef;
    std::string clip_tokenizer_json;
    std::string clip_embedding_lookup_bin;
    std::string clip_proj_w_bin;
    std::string clip_proj_b_bin;

    for (int i = 1; i < argc; i++)
    {
        const std::string a = argv[i];
        auto need = [&](const char *opt) -> const char * {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "Missing value for %s\n", opt);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--media") media_json = need("--media");
        else if (a == "--profile") profile_arg = need("--profile");
        else if (a == "--model") model_path = need("--model");
        else if (a == "--detector-model") detector_model_path = need("--detector-model");
        else if (a == "--det-post-file") det_post_cfg_file = need("--det-post-file");
        else if (a == "--det-post-json") det_post_cfg_json = need("--det-post-json");
        else if (a == "--udp") udp_arg = need("--udp");
        else if (a == "--post-type") post_type = need("--post-type");
        else if (a == "--post-json") post_cfg_json = need("--post-json");
        else if (a == "--post-file") post_cfg_file = need("--post-file");
        else if (a == "--classification-labels-json") classification_labels_json = need("--classification-labels-json");
        else if (a == "--preprocess-color") preprocess_color = need("--preprocess-color");
        else if (a == "--dpm") dpm = true;
        else if (a == "--dpm-mode") dpm_mode = need("--dpm-mode");
        else if (a == "--dpm-labels") dpm_labels = need("--dpm-labels");
        else if (a == "--dpm-max-rois") dpm_max_rois = (uint32_t)std::max(0, std::atoi(need("--dpm-max-rois")));
        else if (a == "--dpm-smooth-alpha") dpm_smooth_alpha = std::strtof(need("--dpm-smooth-alpha"), nullptr);
        else if (a == "--dpm-mask-size") dpm_mask_size = (uint32_t)std::max(1, std::atoi(need("--dpm-mask-size")));
        else if (a == "--dpm-block-size") dpm_block_size = (uint32_t)std::max(0, std::atoi(need("--dpm-block-size")));
        else if (a == "--dpm-attach") dpm_attach = true;
        else if (a == "--dpm-attach-recttest") { dpm_attach = true; dpm_recttest = true; }
        else if (a == "--verbose") verbose = true;
        else if (a == "--clip-text-default") clip_text_default = true;
        else if (a == "--clip-text-hef") clip_text_hef = need("--clip-text-hef");
        else if (a == "--clip-tokenizer") clip_tokenizer_json = need("--clip-tokenizer");
        else if (a == "--clip-embed-lookup") clip_embedding_lookup_bin = need("--clip-embed-lookup");
        else if (a == "--clip-proj-w") clip_proj_w_bin = need("--clip-proj-w");
        else if (a == "--clip-proj-b") clip_proj_b_bin = need("--clip-proj-b");
        else if (a == "--help" || a == "-h")
        {
            std::printf(
                "Usage: %s --media <json> --profile <name_or_idx> --model <hef> [--detector-model <hef>] --udp <host:port> [--post-type <t>] [--post-file <path>] [--post-json <json>] [--dpm ...] [--dpm-attach] [--clip-text-default | --clip-text-hef ...] [--verbose]\n"
                "  --post-type  auto|detection|classification|clip|segmentation|keypoint|embedding|depth|ocr|none\n"
                "               (ocr: --post-file/--post-json with backend_lib_path, backend_function, backend_config_path)\n"
                "  --preprocess-color  auto|none|nv12_to_rgb|nv12_to_bgr\n"
                "               (Used by HAL_INFERENCE_OPS.tensor_from_frame() for single-input models; default auto picks NV12->RGB when needed.)\n"
                "  --classification-labels-json <path>  Torchvision-style imagenet_class_index.json (optional; fills classification labels)\n"
                "  --clip-text-default  Use built-in CLIP text encoder paths (see hal_clip_text_encoder default_config).\n",
                argv[0]);
            return 0;
        }
    }

    if (model_path.empty())
    {
        std::fprintf(stderr, "Error: require --model (--media is optional; omit to use the HAL embedded default)\n");
        return 2;
    }

    std::string host;
    uint16_t port = 0;
    if (!parse_host_port(udp_arg, host, port))
    {
        std::fprintf(stderr, "Error: invalid --udp, expected host:port\n");
        return 2;
    }

    std::signal(SIGINT, on_sig);
    std::signal(SIGTERM, on_sig);

    CliState cli_state;
    AppCtx ctx;
    ctx.cli_state = &cli_state;
    ctx.verbose = verbose;
    ctx.dpm.enabled = dpm;
    ctx.dpm.use_attach_api = dpm_attach;
    ctx.dpm.recttest = dpm_recttest;
    if (dpm_attach)
    {
        // Cache the first label for the per-frame attach call (avoids per-frame allocation).
        const auto labels = split_csv_trim(dpm_labels);
        std::snprintf(ctx.dpm.attach_label, sizeof(ctx.dpm.attach_label), "%s",
                      labels.empty() ? "person" : labels.front().c_str());
    }
    ctx.preprocess_color_choice = preprocess_choice_from_str(preprocess_color);
    {
        const std::string act = extract_json_string_key_best_effort(post_cfg_json, "output_activation");
        if (act == "softmax")
            ctx.cls_softmax = true;
        const float tk = extract_json_float_key_best_effort(post_cfg_json, "top_k", 0.0f);
        if (std::isfinite(tk) && tk > 0.5f)
            ctx.cls_top_k = (uint32_t)std::max(1, (int)(tk + 0.5f));
    }
    if (!classification_labels_json.empty())
    {
        if (!load_imagenet_class_index_json(classification_labels_json.c_str(), ctx.cls_label_table))
            std::fprintf(stderr, "Warning: --classification-labels-json failed to load/parse: %s\n",
                         classification_labels_json.c_str());
    }
    if (ctx.dpm.enabled)
    {
        ctx.dpm.labels_csv = dpm_labels;
        ctx.dpm.max_rois = dpm_max_rois ? dpm_max_rois : 35U;
        ctx.dpm.smooth_alpha = dpm_smooth_alpha;
        ctx.dpm.mask_size = dpm_mask_size ? dpm_mask_size : 128U;
        ctx.dpm.mosaic_block_size = dpm_block_size;
        if (dpm_mode == "blur")
            ctx.dpm.render_mode = DpmRenderMode::Blur;
        else if (dpm_mode == "overlay")
            ctx.dpm.render_mode = DpmRenderMode::Overlay;
        else
            ctx.dpm.render_mode = DpmRenderMode::Mosaic;
    }

    HalMediaConfig mcfg{};
    mcfg.config_path = media_json.empty() ? nullptr : media_json.c_str();
    mcfg.config_json = nullptr;
    mcfg.image_config = {};

    int rc = HAL_MEDIA_OPS.init(&mcfg, &ctx.media_ctx);
    if (rc != HAL_OK || !ctx.media_ctx)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.init failed rc=%d\n", rc);
        return 1;
    }

    if (!profile_arg.empty())
    {
        if (is_number(profile_arg))
        {
            char *plist[64]{};
            uint32_t pcount = 0;
            rc = HAL_MEDIA_OPS.get_profile_list(ctx.media_ctx, plist, &pcount);
            const uint32_t idx = (uint32_t)std::atoi(profile_arg.c_str());
            if (rc == HAL_OK && idx < pcount && plist[idx])
                (void)HAL_MEDIA_OPS.switch_profile(ctx.media_ctx, plist[idx], false);
        }
        else
        {
            (void)HAL_MEDIA_OPS.switch_profile(ctx.media_ctx, profile_arg.c_str(), false);
        }
    }

    (void)HAL_MEDIA_OPS.set_encoder_auto_feed(ctx.media_ctx, false);

    // pick codec (first H264/H265)
    void *codec_list_raw = nullptr;
    uint32_t codec_count = 0;
    rc = HAL_MEDIA_OPS.get_codec_list(ctx.media_ctx, &codec_list_raw, &codec_count);
    auto **codec_list = reinterpret_cast<void **>(codec_list_raw);
    if (rc != HAL_OK || !codec_list || codec_count == 0)
    {
        std::fprintf(stderr, "get_codec_list failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }
    HalCodecContext *selected_codec = nullptr;
    for (uint32_t i = 0; i < codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(codec_list[i]);
        if (!cc) continue;
        if (cc->config.packet_type == HAL_PACKET_TYPE_H264 || cc->config.packet_type == HAL_PACKET_TYPE_H265)
        {
            selected_codec = cc;
            break;
        }
    }
    if (!selected_codec)
        selected_codec = static_cast<HalCodecContext *>(codec_list[0]);
    ctx.codec_ctx = selected_codec;

    // init udp
    HalCodecConfig ccfg{};
    (void)HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
    HalUdpStreamConfig ucfg{};
    ucfg.host = host.c_str();
    ucfg.port = port;
    ucfg.mode = (ccfg.packet_type == HAL_PACKET_TYPE_H265) ? HalUdpStreamMode::RtpH265AnnexB : HalUdpStreamMode::RtpH264AnnexB;
    HalUdpStream udp(ucfg);
    ctx.udp = &udp;

    // init dsp
    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    rc = HAL_DSP_OPS.init(&dcfg, &ctx.dsp_ctx);
    if (rc != HAL_OK || !ctx.dsp_ctx)
    {
        std::fprintf(stderr, "HAL_DSP_OPS.init failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    // init inference
    HalInferenceConfig icfg{};
    std::snprintf(icfg.model_path, sizeof(icfg.model_path), "%s", model_path.c_str());
    icfg.batch_size = 1;
    icfg.timeout_ms = 1000;
    icfg.use_dma = true;
    icfg.platform_config = nullptr;
    icfg.platform_data = nullptr;
    icfg.preprocess = make_preprocess_cfg(HAL_PREPROCESS_COLOR_NONE);
    ctx.infer = HAL_INFERENCE_OPS.create(&icfg);
    if (!ctx.infer)
    {
        std::fprintf(stderr, "HAL_INFERENCE_OPS.create failed\n");
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    // Best-effort preprocess selection:
    // - Note: on Hailo-15, HAL_INFERENCE_OPS.tensor_from_frame() is NV12 passthrough only (no color conversion).
    // - We keep this block for compatibility, but NV12->RGB/BGR conversion for single-input models is done
    //   in the example (DSP convert_format) when --preprocess-color requests it.
    {
        const auto choice = preprocess_choice_from_str(preprocess_color);
        HalPreprocessColor desired = HAL_PREPROCESS_COLOR_NONE;
        if (choice == PreprocessColorChoice::Auto)
            desired = choose_preprocess_color_auto(ctx.infer);
        else if (choice == PreprocessColorChoice::None)
            desired = HAL_PREPROCESS_COLOR_NONE;
        else if (choice == PreprocessColorChoice::Nv12ToRgb)
            desired = HAL_PREPROCESS_COLOR_NV12_TO_RGB;
        else if (choice == PreprocessColorChoice::Nv12ToBgr)
            desired = HAL_PREPROCESS_COLOR_NV12_TO_BGR;

        if (desired != HAL_PREPROCESS_COLOR_NONE && model_prefers_tensor_from_frame(ctx.infer))
        {
            HAL_INFERENCE_OPS.destroy(ctx.infer);
            ctx.infer = nullptr;
            icfg.preprocess = make_preprocess_cfg(desired);
            ctx.infer = HAL_INFERENCE_OPS.create(&icfg);
            if (!ctx.infer)
            {
                std::fprintf(stderr, "HAL_INFERENCE_OPS.create failed (preprocess)\n");
                HAL_DSP_OPS.deinit(ctx.dsp_ctx);
                HAL_MEDIA_OPS.deinit(ctx.media_ctx);
                return 1;
            }
            HAL_LOG_INFO("ai_example_v2: enabled preprocess color=%d for model=%s",
                         (int)desired, model_path.c_str());
        }
    }
    if (auto hw = model_input_hw(ctx.infer))
    {
        ctx.model_w = hw->first;
        ctx.model_h = hw->second;
    }

    // Auto mode detection for face-landmarks: use two-model (detector + landmarks) like v1.
    const bool is_face_landmarks =
        (model_path.find("face_landmarks") != std::string::npos) ||
        (ctx.model_w == 192 && ctx.model_h == 192);
    ctx.two_model_landmarks = is_face_landmarks;
    if (ctx.dpm.enabled)
    {
        // DPM uses a dedicated detector + segmentation model; do not auto-enable landmarks mode.
        ctx.two_model_landmarks = false;
        post_type = "segmentation";
    }
    if (post_type == "auto")
        post_type = is_face_landmarks ? "keypoint" : "detection";

    if (ctx.two_model_landmarks || ctx.dpm.enabled)
    {
        HalInferenceConfig dcfg{};
        std::snprintf(dcfg.model_path, sizeof(dcfg.model_path), "%s", detector_model_path.c_str());
        dcfg.batch_size = 1;
        dcfg.timeout_ms = 1000;
        dcfg.use_dma = true;
        dcfg.platform_config = nullptr;
        dcfg.platform_data = nullptr;
        dcfg.preprocess = {};
        ctx.det_infer = HAL_INFERENCE_OPS.create(&dcfg);
        if (ctx.det_infer)
        {
            if (auto dhw = model_input_hw(ctx.det_infer))
            {
                ctx.det_w = dhw->first;
                ctx.det_h = dhw->second;
            }
            HalPostprocessConfig pc{};
            std::memset(&pc, 0, sizeof(pc));
            pc.type = HAL_POST_TYPE_DETECTION;
            pc.config.detection.config_file = det_post_cfg_file.empty() ? nullptr : det_post_cfg_file.c_str();
            pc.config.detection.config_json = det_post_cfg_json.empty() ? nullptr : det_post_cfg_json.c_str();
            ctx.det_post = HAL_POSTPROCESS_OPS.create(&pc);
        }
        if (ctx.two_model_landmarks)
        {
            HAL_LOG_INFO("ai_example_v2: two-model landmarks enabled (detector=%s det_in=%ux%u landmarks_in=%ux%u)",
                         detector_model_path.c_str(), ctx.det_w, ctx.det_h, ctx.model_w, ctx.model_h);
        }
        else if (ctx.dpm.enabled)
        {
            HAL_LOG_INFO("ai_example_v2: DPM enabled (detector=%s det_post_file=%s seg_model=%s seg_in=%ux%u)",
                         detector_model_path.c_str(),
                         det_post_cfg_file.c_str(),
                         model_path.c_str(),
                         ctx.model_w, ctx.model_h);
        }
    }

    // init postprocess (optional)
    std::memset(&ctx.post_cfg, 0, sizeof(ctx.post_cfg));
    if (post_type != "none")
    {
        if (post_type == "detection") ctx.post_cfg.type = HAL_POST_TYPE_DETECTION;
        else if (post_type == "classification") ctx.post_cfg.type = HAL_POST_TYPE_CLASSIFICATION;
        else if (post_type == "clip") ctx.post_cfg.type = HAL_POST_TYPE_CLIP;
        else if (post_type == "segmentation") ctx.post_cfg.type = HAL_POST_TYPE_SEGMENTATION;
        else if (post_type == "keypoint") ctx.post_cfg.type = HAL_POST_TYPE_KEYPOINT;
        else if (post_type == "embedding") ctx.post_cfg.type = HAL_POST_TYPE_EMBEDDING;
        else if (post_type == "depth") ctx.post_cfg.type = HAL_POST_TYPE_DEPTH;
        else if (post_type == "ocr" || post_type == "ocr-recognition" || post_type == "ocr_rec")
            ctx.post_cfg.type = HAL_POST_TYPE_OCR_RECOGNITION;
        else if (post_type == "ocr-detection" || post_type == "ocr_det")
            ctx.post_cfg.type = HAL_POST_TYPE_OCR_DETECTION;
        else ctx.post_cfg.type = HAL_POST_TYPE_DETECTION;

        if (ctx.post_cfg.type == HAL_POST_TYPE_CLIP)
            hal_clip_postprocess_config_init(&ctx.post_cfg.config.clip);
        if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_DETECTION)
            hal_ocr_detection_post_config_init(&ctx.post_cfg.config.ocr_detection);
        if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
            hal_ocr_recognition_post_config_init(&ctx.post_cfg.config.ocr_recognition);
        if (ctx.post_cfg.type == HAL_POST_TYPE_DEPTH)
            hal_depth_config_init(&ctx.post_cfg.config.depth);

        // Best-effort: pass config file/json to the matching union field.
        if (!post_cfg_file.empty())
        {
            if (ctx.post_cfg.type == HAL_POST_TYPE_DETECTION) ctx.post_cfg.config.detection.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_CLASSIFICATION) ctx.post_cfg.config.classification.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_CLIP) ctx.post_cfg.config.clip.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_SEGMENTATION) ctx.post_cfg.config.segmentation.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_KEYPOINT) ctx.post_cfg.config.keypoint.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_EMBEDDING) ctx.post_cfg.config.embedding.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_DETECTION)
                ctx.post_cfg.config.ocr_detection.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
                ctx.post_cfg.config.ocr_recognition.config_file = post_cfg_file.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_DEPTH)
                ctx.post_cfg.config.depth.config_file = post_cfg_file.c_str();
        }
        if (!post_cfg_json.empty())
        {
            if (ctx.post_cfg.type == HAL_POST_TYPE_DETECTION) ctx.post_cfg.config.detection.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_CLASSIFICATION) ctx.post_cfg.config.classification.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_CLIP) ctx.post_cfg.config.clip.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_SEGMENTATION) ctx.post_cfg.config.segmentation.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_KEYPOINT) ctx.post_cfg.config.keypoint.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_EMBEDDING) ctx.post_cfg.config.embedding.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_DETECTION)
                ctx.post_cfg.config.ocr_detection.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_OCR_RECOGNITION)
                ctx.post_cfg.config.ocr_recognition.config_json = post_cfg_json.c_str();
            else if (ctx.post_cfg.type == HAL_POST_TYPE_DEPTH)
                ctx.post_cfg.config.depth.config_json = post_cfg_json.c_str();
        }
        ctx.post = HAL_POSTPROCESS_OPS.create(&ctx.post_cfg);
    }

    // Optional: on-device CLIP text encode (align with `apps/clip` text encoder logic).
    // Enabled with --clip-text-default (HalClipTextEncoder::init()) or when all --clip-* resource paths are set;
    // Populate prompts via HalClipPostprocessConfig (--post-json merges into struct via hal_clip_postprocess_config_merge_json).
    {
        const bool explicit_clip_paths = !clip_text_hef.empty() && !clip_tokenizer_json.empty() &&
                                         !clip_embedding_lookup_bin.empty() && !clip_proj_w_bin.empty() &&
                                         !clip_proj_b_bin.empty();
        const bool want_clip_text = clip_text_default || explicit_clip_paths;

        if (want_clip_text)
        {
            HalClipPostprocessConfig probe{};
            hal_clip_postprocess_config_init(&probe);
            if (!post_cfg_json.empty())
                hal_v2::hal_clip_postprocess_config_merge_json(&probe, post_cfg_json.c_str());
            const bool have_pos_neg = (probe.positive_prompt[0] != '\0');
            const bool have_zero_shot = (probe.num_zero_shot_prompts > 0U);

            if (have_pos_neg || have_zero_shot)
            {
                int trc = HAL_ERR_RESULT;
                if (explicit_clip_paths)
                {
                    hal_v2::HalClipTextEncoderConfig tcfg{};
                    tcfg.hef_path = clip_text_hef;
                    tcfg.tokenizer_json_path = clip_tokenizer_json;
                    tcfg.embedding_lookup_bin_path = clip_embedding_lookup_bin;
                    tcfg.projection_weights_bin_path = clip_proj_w_bin;
                    tcfg.projection_bias_bin_path = clip_proj_b_bin;
                    tcfg.context_length = 77;
                    trc = ctx.clip_prompt.text.init(tcfg);
                }
                else if (clip_text_default)
                    trc = ctx.clip_prompt.text.init();
                else
                    trc = HAL_ERR_INVALID_ARG;
                if (trc == HAL_OK && ctx.clip_prompt.text.is_ready())
                {
                    hal_clip_postprocess_config_init(&ctx.clip_prompt.clip_cfg);
                    if (!post_cfg_json.empty())
                        hal_v2::hal_clip_postprocess_config_merge_json(&ctx.clip_prompt.clip_cfg, post_cfg_json.c_str());

                    const float score_th = extract_json_float_key_best_effort(post_cfg_json, "score_threshold", 0.8f);
                    const std::string pol_str = extract_json_string_key_best_effort(post_cfg_json, "match_policy");
                    ctx.clip_prompt.clip_cfg.score_threshold = score_th;
                    ctx.clip_prompt.clip_cfg.match_policy = clip_policy_kind_from_cpp(
                        hal_v2::hal_clip_match_policy_from_string(pol_str.empty() ? std::string("softmax") : pol_str));

                    if (have_zero_shot && !have_pos_neg)
                    {
                        ctx.clip_prompt.clip_cfg.positive_prompt[0] = '\0';
                        ctx.clip_prompt.clip_cfg.num_negative_prompts = 0U;
                    }

                    const int cfg_rc = ctx.clip_prompt.scorer.configure(ctx.clip_prompt.clip_cfg);
                    ctx.clip_prompt.enabled = (cfg_rc == HAL_OK && ctx.clip_prompt.scorer.ready());
                    if (!ctx.clip_prompt.enabled)
                        HAL_LOG_ERROR("ai_example_v2: HalClipPromptScorer::configure failed rc=%d", cfg_rc);
                    else
                    {
                        HAL_LOG_INFO("ai_example_v2: clip text encoder ready (pos_neg=%d prompts=%u dim=%u)",
                                     (int)ctx.clip_prompt.scorer.positive_negative_mode(),
                                     ctx.clip_prompt.scorer.positive_negative_mode()
                                         ? (1U + ctx.clip_prompt.clip_cfg.num_negative_prompts)
                                         : ctx.clip_prompt.clip_cfg.num_zero_shot_prompts,
                                     ctx.clip_prompt.text.embedding_dim());
                    }
                }
                else
                {
                    HAL_LOG_ERROR("ai_example_v2: clip text encoder init failed rc=%d", trc);
                }
            }
            else if (verbose)
            {
                HAL_LOG_INFO(
                    "ai_example_v2: clip text encoder args provided, but no positive_prompt/prompts in --post-json");
            }
        }
    }

    hal_draw_config_init_default(&ctx.draw_cfg);
    if (ctx.post_cfg.type == HAL_POST_TYPE_DEPTH)
    {
        ctx.draw_cfg.draw_depth_colormap = true;
        ctx.draw_cfg.depth_colormap_alpha = 0.55f;
        ctx.draw_cfg.depth_thumbnail_max_width = 160;
        ctx.draw_cfg.depth_thumbnail_margin = 10;
    }
    if (ctx.two_model_landmarks)
    {
        // V1-aligned: render landmarks as points (no skeleton lines).
        ctx.draw_cfg.default_keypoint_radius = 3;
        ctx.draw_cfg.default_keypoint_color = HalColor{0, 255, 0, 255};
        ctx.draw_cfg.draw_keypoints = true;
        ctx.draw_cfg.draw_skeleton = false;
    }

    // pick streams
    void *video_list_raw = nullptr;
    uint32_t video_count = 0;
    rc = HAL_MEDIA_OPS.get_video_list(ctx.media_ctx, &video_list_raw, &video_count);
    auto **video_list = reinterpret_cast<void **>(video_list_raw);
    if (rc != HAL_OK || !video_list || video_count == 0)
    {
        std::fprintf(stderr, "get_video_list failed rc=%d\n", rc);
        if (ctx.post) HAL_POSTPROCESS_OPS.destroy(ctx.post);
        HAL_INFERENCE_OPS.destroy(ctx.infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }
    // Preview/original stream: choose by encoder WxH first, otherwise feeding encoder may produce no output.
    uint32_t want_w = ccfg.width;
    uint32_t want_h = ccfg.height;
    if (want_w == 0 || want_h == 0)
    {
        // fallback to model size if encoder config is not populated
        want_w = ctx.model_w;
        want_h = ctx.model_h;
    }
    // NOTE: Do not force preview/encode resolution for DPM here.
    // Some profiles have only one encoder stream (e.g., 4K). Picking a different video sink without a matching
    // encoder will result in pkts_out=0 due to WxH mismatch.
    const uint32_t vid_idx = pick_video_index_exact_or_closest(video_list, video_count, want_w, want_h);
    ctx.video_ctx = video_list[vid_idx];
    auto *vctx = static_cast<HalVideoContext *>(ctx.video_ctx);
    ctx.stream_key = (vctx && vctx->video_name[0]) ? std::string(vctx->video_name) : std::string();
    HAL_LOG_INFO("ai_example_v2: selected video stream_key=\"%s\" %ux%u (encoder=%ux%u model=%ux%u)",
                 ctx.stream_key.c_str(),
                 vctx ? vctx->config.width : 0U,
                 vctx ? vctx->config.height : 0U,
                 ccfg.width, ccfg.height,
                 ctx.model_w, ctx.model_h);

    // Make sure we pick an encoder whose input WxH matches the selected preview stream.
    // On Hailo-15 FROM_MEDIA, feeding a frame whose WxH doesn't match the encoder input usually results in pkts_out=0.
    if (vctx && vctx->config.width && vctx->config.height)
    {
        HalCodecContext *best = nullptr;
        for (uint32_t i = 0; i < codec_count; i++)
        {
            auto *c = static_cast<HalCodecContext *>(codec_list[i]);
            if (!c) continue;
            if (c->config.packet_type != ccfg.packet_type) continue; // keep RTP mode stable
            if (c->config.width == vctx->config.width && c->config.height == vctx->config.height)
            {
                // Prefer encoder id matching the stream_key (often "sinkX"), otherwise just match WxH.
                if (!best || (ctx.stream_key == std::string(c->codec_name)))
                    best = c;
            }
        }
        if (best && best != ctx.codec_ctx)
        {
            ctx.codec_ctx = best;
            (void)HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
            HAL_LOG_INFO("ai_example_v2: selected codec=\"%s\" %ux%u packet_type=%d (match stream_key=\"%s\")",
                         best->codec_name, ccfg.width, ccfg.height, (int)ccfg.packet_type, ctx.stream_key.c_str());
        }
        else if (!best)
        {
            HAL_LOG_WARNING("ai_example_v2: no encoder matches preview %ux%u; pkts_out may be 0",
                            vctx->config.width, vctx->config.height);
        }
    }

    (void)HAL_CODEC_OPS.subscribe(ctx.codec_ctx, codec_cb, &ctx);

    // AI stream: pick closest to model input (prefer exact). If it matches preview stream, subscribe once and share frames.
    uint32_t ai_idx = pick_video_index_exact_or_closest_prefer_not(video_list, video_count, ctx.model_w, ctx.model_h, vid_idx);
    ctx.ai_video_ctx = video_list[ai_idx];
    auto *aiv = static_cast<HalVideoContext *>(ctx.ai_video_ctx);
    ctx.ai_stream_key = (aiv && aiv->video_name[0]) ? std::string(aiv->video_name) : std::string();

    const bool same_stream = (ctx.ai_video_ctx == ctx.video_ctx) && (ctx.ai_stream_key == ctx.stream_key);
    if (same_stream)
    {
        HAL_LOG_INFO("ai_example_v2: subscribe shared stream_key=\"%s\" for preview+ai", ctx.stream_key.c_str());
        rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx, ctx.stream_key.c_str(), video_cb_preview_and_ai, &ctx);
    }
    else
    {
        HAL_LOG_INFO("ai_example_v2: subscribe preview stream_key=\"%s\" and ai stream_key=\"%s\"",
                     ctx.stream_key.c_str(), ctx.ai_stream_key.c_str());
        rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx, ctx.stream_key.c_str(), video_cb_preview_only, &ctx);
        if (rc == HAL_OK)
            rc = HAL_VIDEO_OPS.subscribe_stream(ctx.ai_video_ctx, ctx.ai_stream_key.c_str(), video_cb_ai_only, &ctx);
    }
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "subscribe_stream failed rc=%d\n", rc);
        (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
        if (ctx.post) HAL_POSTPROCESS_OPS.destroy(ctx.post);
        HAL_INFERENCE_OPS.destroy(ctx.infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    rc = HAL_MEDIA_OPS.start(ctx.media_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.start failed rc=%d\n", rc);
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
        if (!same_stream)
            (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.ai_video_ctx, ctx.ai_stream_key.c_str());
        (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
        if (ctx.post) HAL_POSTPROCESS_OPS.destroy(ctx.post);
        HAL_INFERENCE_OPS.destroy(ctx.infer);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return 1;
    }

    // When using the attach_frame_analytics path, enable the media library DSP blender's dynamic
    // privacy-mask path (dynamic_enabled + masked_labels + label_to_class_id). The blender only
    // consumes per-frame AI metadata when this profile config is on.
    if (ctx.dpm.enabled && ctx.dpm.use_attach_api && HAL_MEDIA_OPS.dynamic_change_image_config)
    {
        HalMediaImageConfig icfg{};
        icfg.privacy_mask = true;
        icfg.digital_zoom = false;
        icfg.digital_zoom_value = 1;
        auto &pm = icfg.privacy_mask_config;
        pm.dynamic_enabled = true;
        pm.dilation_size = 4;
        pm.blur_radius = (int)std::clamp<uint32_t>(ctx.dpm.mosaic_block_size ? ctx.dpm.mosaic_block_size : 24, 2, 64);
        const auto labels = split_csv_trim(ctx.dpm.labels_csv);
        uint32_t li = 0;
        for (const auto &lbl : labels)
        {
            if (li >= HAL_PM_MAX_LABELS)
                break;
            std::snprintf(pm.masked_labels[li], sizeof(pm.masked_labels[li]), "%s", lbl.c_str());
            pm.masked_label_class_ids[li] = 0; // single-class segmentor: fg class id 0
            li++;
        }
        pm.num_masked_labels = li;
        int erc = HAL_MEDIA_OPS.dynamic_change_image_config(ctx.media_ctx, &icfg);
        std::printf("dpm-attach: enable dynamic config ret=%d labels=%u blur=%d\n", erc, li, pm.blur_radius);
        if (erc != HAL_OK)
            std::fprintf(stderr, "warning: dynamic_change_image_config failed rc=%d (dynamic mask won't draw)\n", erc);
    }

    ctx.ai_worker = std::thread(ai_worker_loop, &ctx);
    ctx.preview_worker = std::thread(preview_worker_loop, &ctx);

    std::printf("Running. Ctrl+C to stop.\n");
    auto last = std::chrono::steady_clock::now();
    uint64_t last_fi = 0, last_fe = 0, last_pk = 0;
    while (!g_stop.load(std::memory_order_acquire))
    {
        if (g_sig)
        {
            g_stop.store(true, std::memory_order_release);
            ctx.q_cv.notify_all(); // wake ai worker if waiting on empty queue
            ctx.preview_cv.notify_all(); // wake preview worker
            ctx.result.cv.notify_all(); // wake preview wait on results
            break;
        }
        auto now = std::chrono::steady_clock::now();
        if (now - last >= std::chrono::seconds(1))
        {
            const uint64_t fi = g_frames_in.load(std::memory_order_relaxed);
            const uint64_t afi = g_ai_frames_in.load(std::memory_order_relaxed);
            const uint64_t aok = g_ai_infer_ok.load(std::memory_order_relaxed);
            const uint64_t apok = g_ai_post_ok.load(std::memory_order_relaxed);
            const uint64_t fe = g_frames_encoded_in.load(std::memory_order_relaxed);
            const uint64_t pk = g_pkts_out.load(std::memory_order_relaxed);
            HAL_LOG_INFO("ai_example_v2: frames_in=%lu (+%lu/s) ai_frames_in=%lu ai_infer_ok=%lu ai_post_ok=%lu enc_in=%lu (+%lu/s) pkts_out=%lu (+%lu/s)",
                         (unsigned long)fi, (unsigned long)(fi - last_fi),
                         (unsigned long)afi,
                         (unsigned long)aok,
                         (unsigned long)apok,
                         (unsigned long)fe, (unsigned long)(fe - last_fe),
                         (unsigned long)pk, (unsigned long)(pk - last_pk));
            last_fi = fi;
            last_fe = fe;
            last_pk = pk;
            last = now;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    (void)HAL_MEDIA_OPS.stop(ctx.media_ctx);
    (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
    if (!same_stream)
        (void)HAL_VIDEO_OPS.unsubscribe_stream(ctx.ai_video_ctx, ctx.ai_stream_key.c_str());
    (void)HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_cb);
    ctx.q_cv.notify_all(); // best-effort wake before join
    ctx.preview_cv.notify_all();
    ctx.result.cv.notify_all();
    if (ctx.ai_worker.joinable()) ctx.ai_worker.join();
    if (ctx.preview_worker.joinable()) ctx.preview_worker.join();
    if (ctx.post) HAL_POSTPROCESS_OPS.destroy(ctx.post);
    if (ctx.infer) HAL_INFERENCE_OPS.destroy(ctx.infer);
    (void)HAL_DSP_OPS.deinit(ctx.dsp_ctx);
    (void)HAL_MEDIA_OPS.deinit(ctx.media_ctx);
    return 0;
}

