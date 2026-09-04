/**
 * @file test_media_all_func.cpp
 * @brief Interactive exercise of HAL media / video / codec ops (Hailo-15 MediaLibrary).
 *
 * Defaults: init media, print all frontend + encoder streams, RTP/UDP push on the first H.264/H.265 encoder.
 * Switch push encoder: udp_select / udp_push <codec_idx> [host] [port] (indices from "streams" encoder list).
 *
 * Usage:
 *   hal-test-media-all-func <medialib_json_path> [udp_host] [udp_port]
 *
 * udp_host defaults to 127.0.0.1, udp_port to 5004.
 *
 * Interactive: after `streams`, use e.g. `video_sensor_info 0` to query SensorRegistry (FROM_MEDIA).
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"
#include "media/hal_codec_internal.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"
#include "media/hal_isp.h"
#include "media/hal_osd.h"

#include "common/hal_buffer.h"

#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <algorithm>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <strings.h>

namespace
{

constexpr size_t MAX_LINE = 512;
constexpr int HISTORY_MAX = 50;

std::atomic<bool> g_shutdown{false};
volatile sig_atomic_t g_signal_pending = 0;

void on_signal(int)
{
    g_signal_pending = 1;
}

void install_signal_handlers()
{
    struct sigaction sa
    {
    };
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    (void)sigaction(SIGINT, &sa, nullptr);
    (void)sigaction(SIGTERM, &sa, nullptr);
}

void log_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    char buf[2048];
    (void)std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    HAL_LOG_ERROR("%s", buf);
}

const char *status_str(HalStatus s)
{
    switch (s)
    {
        case HAL_STATUS_UNINITIALIZED:
            return "UNINITIALIZED";
        case HAL_STATUS_INITIALIZED:
            return "INITIALIZED";
        case HAL_STATUS_RUNNING:
            return "RUNNING";
        case HAL_STATUS_STOPPED:
            return "STOPPED";
        case HAL_STATUS_ERROR:
            return "ERROR";
        default:
            return "?";
    }
}

const char *packet_type_str(HalPacketType pt)
{
    switch (pt)
    {
        case HAL_PACKET_TYPE_H264:
            return "H264";
        case HAL_PACKET_TYPE_H265:
            return "H265";
        case HAL_PACKET_TYPE_MJPEG:
            return "MJPEG";
        case HAL_PACKET_TYPE_DATA:
            return "DATA";
        default:
            return "?";
    }
}

const char *rc_str(HalRateControlMode m)
{
    switch (m)
    {
        case HAL_RC_CBR:
            return "CBR";
        case HAL_RC_VBR:
            return "VBR";
        case HAL_RC_CVBR:
            return "CVBR";
        case HAL_RC_CQP:
            return "CQP";
        default:
            return "?";
    }
}

/* ------------------------------------------------------------------ */
/* App state                                                          */
/* ------------------------------------------------------------------ */

void *g_media_ctx = nullptr;
HalMediaImageConfig g_image_cfg{};

/** Privacy mask regions (multiple, keyed by id — mirrors webserver's map<id,polygon>).
 *  Static masks: up to MAX_NUM_OF_STATIC_PRIVACY_MASKS (8) regions, each up to 8 vertices. */
struct PrivacyRegion
{
    std::string id;
    bool enabled = true;
    /* up to 8 normalized [0..1] vertices; display-space (post rotation/flip), as on screen */
    std::vector<std::pair<float, float>> points;
};
static std::vector<PrivacyRegion> g_pm_regions;
/* Stable backing storage for HalPrivacyMaskItem.id/name pointers handed to the HAL.
 * Rebuilt by sync_privacy_regions_to_cfg() before every apply; the HAL copies the id into its
 * own std::string-keyed maps during the call, so the pointers only need to outlive the call. */
static std::vector<std::string> g_pm_ids;
static std::vector<HalPrivacyMaskItem> g_pm_items;

/** Rebuild g_image_cfg.privacy_mask_config.items from g_pm_regions.
 *  Mirrors webserver norm_to_absolut() ordering: vertices are kept in display space and the HAL
 *  applies the reverse-flip/rotate to store the original polygon (see hailo15_media_impl.cpp). */
static void sync_privacy_regions_to_cfg()
{
    g_pm_ids.clear();
    g_pm_items.clear();
    size_t n = 0;
    for (const auto &r : g_pm_regions)
    {
        if (r.enabled && r.points.size() >= 3U)
        {
            ++n;
        }
    }
    g_pm_ids.reserve(n);
    g_pm_items.reserve(n);
    for (const auto &r : g_pm_regions)
    {
        if (!r.enabled || r.points.size() < 3U)
        {
            continue;
        }
        g_pm_ids.push_back(r.id);
        HalPrivacyMaskItem it{};
        const char *sid = g_pm_ids.back().c_str();
        it.id = sid;
        it.name = sid;
        it.is_enabled = true;
        int pi = 0;
        for (; pi < 8 && static_cast<size_t>(pi) < r.points.size(); ++pi)
        {
            it.points[pi].x = std::clamp(r.points[static_cast<size_t>(pi)].first, 0.0F, 1.0F);
            it.points[pi].y = std::clamp(r.points[static_cast<size_t>(pi)].second, 0.0F, 1.0F);
        }
        if (pi < 8)
        {
            it.points[pi].x = -1.0f; /* terminator: first unused slot */
            it.points[pi].y = 0.0f;
        }
        g_pm_items.push_back(it);
    }
    g_image_cfg.privacy_mask_config.items = g_pm_items.data();
    g_image_cfg.privacy_mask_config.item_count = static_cast<uint32_t>(g_pm_items.size());
}

static PrivacyRegion *find_privacy_region(const std::string &id)
{
    for (auto &r : g_pm_regions)
    {
        if (r.id == id)
        {
            return &r;
        }
    }
    return nullptr;
}

void **g_video_list = nullptr;
uint32_t g_video_count = 0;
void **g_codec_list = nullptr;
uint32_t g_codec_count = 0;

/* Dynamic privacy mask test state (upper-layer-driven: the video callback below calls
 * HalMediaOps.attach_frame_analytics with this bbox each frame when enabled). */
struct
{
    bool enabled{false};
    float x{0.f}, y{0.f}, w{0.f}, h{0.f};
    char label[HAL_PM_LABEL_LEN]{"person"};
} g_dpm_test;
static bool g_dpm_subscribed = false;

static void dpm_video_cb(void * /*video_ctx*/, HalFrameBuffer *frame, void * /*userdata*/)
{
    if (!g_dpm_test.enabled || !g_media_ctx || !frame || !HAL_MEDIA_OPS.attach_frame_analytics)
    {
        return;
    }
    HalFrameDetection d{};
    std::snprintf(d.label, sizeof(d.label), "%s", g_dpm_test.label);
    d.x = g_dpm_test.x;
    d.y = g_dpm_test.y;
    d.w = g_dpm_test.w;
    d.h = g_dpm_test.h;
    d.score = 1.0f;
    /* detection-only (seg_count=0). Real segmentation masks attach via the segs arg. */
    (void)HAL_MEDIA_OPS.attach_frame_analytics(g_media_ctx, frame, &d, 1, nullptr, 0);
}

HalUdpStream *g_udp = nullptr;
int g_udp_codec_index = -1;
void (*g_udp_cb)(void *, HalPacketBuffer *, void *) = nullptr;

/** Annex B: H.264 IDR / SPS (NAL types 5 and 7), aligned with platform helper. */
static bool annex_b_h264_keyframe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 5)
    {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; i++)
    {
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            uint8_t nal = static_cast<uint8_t>(data[i + 3] & 0x1f);
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
        if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            uint8_t nal = static_cast<uint8_t>(data[i + 4] & 0x1f);
            if (nal == 5 || nal == 7)
            {
                return true;
            }
        }
    }
    return false;
}

/** Annex B: H.265 IDR NAL types 19 / 20. */
static bool annex_b_h265_keyframe(const uint8_t *data, uint32_t size)
{
    if (!data || size < 6)
    {
        return false;
    }
    for (uint32_t i = 0; i + 4 < size; i++)
    {
        uint32_t sc = 0;
        if (data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 1)
        {
            sc = 3;
        }
        else if (i + 5 < size && data[i] == 0 && data[i + 1] == 0 && data[i + 2] == 0 && data[i + 3] == 1)
        {
            sc = 4;
        }
        else
        {
            continue;
        }
        const uint32_t off = i + sc;
        if (off >= size)
        {
            continue;
        }
        const uint8_t nal_type = static_cast<uint8_t>((data[off] >> 1) & 0x3f);
        if (nal_type == 19 || nal_type == 20)
        {
            return true;
        }
    }
    return false;
}

struct CodecUdpStats
{
    uint64_t pkt_count = 0;
    bool have_ts = false;
    uint64_t last_ts_ns = 0;
    /** Number of times timestamp_ns is not strictly increasing (<= previous frame). */
    uint64_t ts_not_strictly_increasing = 0;

    uint32_t pkts_since_idr = 0;
    bool seen_idr = false;
    uint64_t last_idr_ts_ns = 0;
    uint64_t idr_seen = 0;
    uint64_t idr_interval_count = 0;
    uint64_t idr_interval_sum_pkts = 0;
    uint64_t idr_interval_min_pkts = UINT64_MAX;
    uint64_t idr_interval_max_pkts = 0;
    uint64_t idr_interval_sum_ns = 0;
    uint64_t idr_interval_min_ns = UINT64_MAX;
    uint64_t idr_interval_max_ns = 0;
};

/** Log first @a kCodecTsOrderWarnBurst violations, then every @a kCodecTsOrderWarnEvery -th (same counter). */
static constexpr uint64_t kCodecTsOrderWarnBurst = 5;
static constexpr uint64_t kCodecTsOrderWarnEvery = 512;

static std::mutex g_codec_udp_stats_mu;
static CodecUdpStats g_codec_udp_stats{};

static void reset_codec_udp_stats()
{
    std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
    g_codec_udp_stats = CodecUdpStats{};
}

void codec_udp_cb(void *codec_ctx, HalPacketBuffer *pkt, void *userdata)
{
    auto *stream = static_cast<HalUdpStream *>(userdata);
    if (!codec_ctx || !pkt || !stream)
    {
        return;
    }
    if (!stream->ok())
    {
        (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
        return;
    }
    if (!pkt->data || pkt->size == 0)
    {
        (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
        return;
    }

    bool ts_order_warn = false;
    uint64_t ts_order_viol_idx = 0;
    uint64_t ts_order_prev_ns = 0;
    uint64_t ts_order_cur_ns = 0;
    uint32_t ts_order_seq = 0;
    {
        auto *cc = static_cast<HalCodecContext *>(codec_ctx);
        std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
        g_codec_udp_stats.pkt_count++;
        if (g_codec_udp_stats.have_ts && pkt->timestamp_ns <= g_codec_udp_stats.last_ts_ns)
        {
            const uint64_t viol = ++g_codec_udp_stats.ts_not_strictly_increasing;
            ts_order_prev_ns = g_codec_udp_stats.last_ts_ns;
            ts_order_cur_ns = pkt->timestamp_ns;
            ts_order_seq = pkt->sequence;
            ts_order_warn =
                (viol <= kCodecTsOrderWarnBurst) || (viol % kCodecTsOrderWarnEvery == 0ULL);
            if (ts_order_warn)
                ts_order_viol_idx = viol;
        }
        g_codec_udp_stats.last_ts_ns = pkt->timestamp_ns;
        g_codec_udp_stats.have_ts = true;

        bool is_key = false;
        if (cc)
        {
            if (cc->config.packet_type == HAL_PACKET_TYPE_H264)
            {
                is_key = annex_b_h264_keyframe(pkt->data, pkt->size);
            }
            else if (cc->config.packet_type == HAL_PACKET_TYPE_H265)
            {
                is_key = annex_b_h265_keyframe(pkt->data, pkt->size);
            }
        }

        if (is_key)
        {
            g_codec_udp_stats.idr_seen++;
            if (g_codec_udp_stats.seen_idr)
            {
                const uint64_t gap_pkts = static_cast<uint64_t>(g_codec_udp_stats.pkts_since_idr) + 1U;
                uint64_t gap_ns = 0;
                if (pkt->timestamp_ns > g_codec_udp_stats.last_idr_ts_ns)
                {
                    gap_ns = pkt->timestamp_ns - g_codec_udp_stats.last_idr_ts_ns;
                }
                g_codec_udp_stats.idr_interval_count++;
                g_codec_udp_stats.idr_interval_sum_pkts += gap_pkts;
                g_codec_udp_stats.idr_interval_min_pkts = std::min(g_codec_udp_stats.idr_interval_min_pkts, gap_pkts);
                g_codec_udp_stats.idr_interval_max_pkts = std::max(g_codec_udp_stats.idr_interval_max_pkts, gap_pkts);
                g_codec_udp_stats.idr_interval_sum_ns += gap_ns;
                g_codec_udp_stats.idr_interval_min_ns = std::min(g_codec_udp_stats.idr_interval_min_ns, gap_ns);
                g_codec_udp_stats.idr_interval_max_ns = std::max(g_codec_udp_stats.idr_interval_max_ns, gap_ns);
            }
            g_codec_udp_stats.seen_idr = true;
            g_codec_udp_stats.last_idr_ts_ns = pkt->timestamp_ns;
            g_codec_udp_stats.pkts_since_idr = 0;
        }
        else
        {
            g_codec_udp_stats.pkts_since_idr++;
        }
    }

    if (ts_order_warn)
    {
        HAL_LOG_WARNING(
            "encoded packet timestamp not strictly increasing (violation #%llu, logs: first %llu then every %llu): "
            "prev_ts_ns=%llu cur_ts_ns=%llu seq=%u",
            static_cast<unsigned long long>(ts_order_viol_idx),
            static_cast<unsigned long long>(kCodecTsOrderWarnBurst),
            static_cast<unsigned long long>(kCodecTsOrderWarnEvery),
            static_cast<unsigned long long>(ts_order_prev_ns), static_cast<unsigned long long>(ts_order_cur_ns),
            static_cast<unsigned>(ts_order_seq));
    }

    std::vector<uint8_t> copy(pkt->data, pkt->data + pkt->size);
    stream->push_annex_b(std::move(copy), pkt->timestamp_ns);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

int refresh_stream_lists()
{
    if (!g_media_ctx)
    {
        return HAL_ERR_INVALID_STATE;
    }
    void *vl = nullptr;
    uint32_t vc = 0;
    void *cl = nullptr;
    uint32_t cc = 0;
    int r1 = HAL_MEDIA_OPS.get_video_list(g_media_ctx, &vl, &vc);
    int r2 = HAL_MEDIA_OPS.get_codec_list(g_media_ctx, &cl, &cc);
    if (r1 != HAL_OK)
    {
        return r1;
    }
    if (r2 != HAL_OK)
    {
        return r2;
    }
    g_video_list = reinterpret_cast<void **>(vl);
    g_video_count = vc;
    g_codec_list = reinterpret_cast<void **>(cl);
    g_codec_count = cc;
    return HAL_OK;
}

void print_all_streams()
{
    std::printf("--- Frontend (video) streams: %u ---\n", static_cast<unsigned>(g_video_count));
    for (uint32_t i = 0; i < g_video_count; i++)
    {
        auto *vc = static_cast<HalVideoContext *>(g_video_list[i]);
        if (!vc)
        {
            std::printf("  [%u] (null)\n", static_cast<unsigned>(i));
            continue;
        }
        HalVideoConfig cfg{};
        if (HAL_VIDEO_OPS.get_current_config(vc, &cfg) == HAL_OK)
        {
            const HalStatus vst = HAL_VIDEO_OPS.get_status(vc);
            std::printf("  [%u] name=%s %ux%u @%ufps fmt=%s pool_max=%u status=%s\n", static_cast<unsigned>(i),
                        vc->video_name, cfg.width, cfg.height, cfg.framerate,
                        hal_pixel_format_to_string(cfg.format), cfg.pool_max_buffers, status_str(vst));
        }
        else
        {
            std::printf("  [%u] name=%s (get_current_config failed)\n", static_cast<unsigned>(i), vc->video_name);
        }
    }
    std::printf("--- Encoder (codec) streams: %u ---\n", static_cast<unsigned>(g_codec_count));
    for (uint32_t i = 0; i < g_codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(g_codec_list[i]);
        if (!cc)
        {
            std::printf("  [%u] (null)\n", static_cast<unsigned>(i));
            continue;
        }
        const HalStatus cst = static_cast<HalStatus>(HAL_CODEC_OPS.get_status(cc));
        std::printf("  [%u] stream_id=%s type=%s %ux%u @%ufps pkt=%s rc=%s bitrate=%u qp=%u gop=%u status=%s\n",
                    static_cast<unsigned>(i), cc->codec_name,
                    cc->config.type == HAL_CODEC_TYPE_FROM_MEDIA ? "FROM_MEDIA" : "other", cc->config.width,
                    cc->config.height, cc->config.framerate, packet_type_str(cc->config.packet_type),
                    rc_str(cc->config.rc_mode), cc->config.bitrate, cc->config.qp, cc->config.gop_size,
                    status_str(cst));
    }
    std::fflush(stdout);
}

void udp_stop_internal()
{
    if (g_udp_codec_index >= 0 && g_codec_list && static_cast<uint32_t>(g_udp_codec_index) < g_codec_count &&
        g_udp_cb)
    {
        void *ctx = g_codec_list[g_udp_codec_index];
        if (ctx)
        {
            (void)HAL_CODEC_OPS.unsubscribe(ctx, g_udp_cb);
        }
    }
    g_udp_cb = nullptr;
    g_udp_codec_index = -1;
    if (g_udp)
    {
        g_udp->stop_accepting();
        g_udp->shutdown();
        delete g_udp;
        g_udp = nullptr;
    }
    reset_codec_udp_stats();
}

int udp_start_for_index(int idx, const char *host, uint16_t port)
{
    if (!g_codec_list || idx < 0 || static_cast<uint32_t>(idx) >= g_codec_count)
    {
        return HAL_ERR_INVALID_ARG;
    }
    udp_stop_internal();
    auto *cc = static_cast<HalCodecContext *>(g_codec_list[idx]);
    if (!cc)
    {
        return HAL_ERR_INVALID_ARG;
    }
    HalUdpStreamConfig us{};
    us.host = host;
    us.port = port;
    if (cc->config.packet_type == HAL_PACKET_TYPE_H265)
    {
        us.mode = HalUdpStreamMode::RtpH265AnnexB;
        us.rtp_payload_type = 97U;
    }
    else
    {
        us.mode = HalUdpStreamMode::RtpH264AnnexB;
        us.rtp_payload_type = 96U;
    }
    g_udp = new HalUdpStream(us);
    if (!g_udp->ok())
    {
        delete g_udp;
        g_udp = nullptr;
        return HAL_ERROR;
    }
    g_udp_cb = codec_udp_cb;
    int rc = HAL_CODEC_OPS.subscribe(cc, g_udp_cb, g_udp);
    if (rc != HAL_OK)
    {
        g_udp->shutdown();
        delete g_udp;
        g_udp = nullptr;
        g_udp_cb = nullptr;
        return rc;
    }
    g_udp_codec_index = idx;
    std::printf("UDP RTP -> %s:%u using codec[%d] %s (%s)\n", host, static_cast<unsigned>(port), idx, cc->codec_name,
                packet_type_str(cc->config.packet_type));
    std::fflush(stdout);
    return HAL_OK;
}

bool parse_pixel_fmt(const char *s, HalPixelFormat *out)
{
    static const struct
    {
        const char *name;
        HalPixelFormat fmt;
    } k[] = {{"nv12", HAL_PIX_FMT_NV12},   {"nv21", HAL_PIX_FMT_NV21}, {"yuv420p", HAL_PIX_FMT_YUV420P},
             {"yuyv", HAL_PIX_FMT_YUYV},   {"rgb24", HAL_PIX_FMT_RGB24}, {"bgr24", HAL_PIX_FMT_BGR24},
             {"argb32", HAL_PIX_FMT_ARGB32}, {"rgba32", HAL_PIX_FMT_RGBA32}, {"gray8", HAL_PIX_FMT_GRAY8},
             {"raw10", HAL_PIX_FMT_RAW10}, {"raw12", HAL_PIX_FMT_RAW12}};
    for (const auto &e : k)
    {
        if (strcasecmp(s, e.name) == 0)
        {
            *out = e.fmt;
            return true;
        }
    }
    return false;
}

bool parse_rc_mode(const char *s, HalRateControlMode *out)
{
    if (strcasecmp(s, "cbr") == 0)
    {
        *out = HAL_RC_CBR;
        return true;
    }
    if (strcasecmp(s, "vbr") == 0)
    {
        *out = HAL_RC_VBR;
        return true;
    }
    if (strcasecmp(s, "cvbr") == 0)
    {
        *out = HAL_RC_CVBR;
        return true;
    }
    if (strcasecmp(s, "cqp") == 0)
    {
        *out = HAL_RC_CQP;
        return true;
    }
    return false;
}

HalRotationAngle parse_rotation_deg(unsigned d)
{
    switch (d)
    {
        case 0:
            return HAL_ROTATION_ANGLE_0;
        case 90:
            return HAL_ROTATION_ANGLE_90;
        case 180:
            return HAL_ROTATION_ANGLE_180;
        case 270:
            return HAL_ROTATION_ANGLE_270;
        default:
            return HAL_ROTATION_ANGLE_0;
    }
}

HalFlipDirection parse_flip(const char *s)
{
    if (strcasecmp(s, "none") == 0)
    {
        return HAL_FLIP_DIRECTION_NONE;
    }
    if (strcasecmp(s, "h") == 0 || strcasecmp(s, "horizontal") == 0)
    {
        return HAL_FLIP_DIRECTION_HORIZONTAL;
    }
    if (strcasecmp(s, "v") == 0 || strcasecmp(s, "vertical") == 0)
    {
        return HAL_FLIP_DIRECTION_VERTICAL;
    }
    if (strcasecmp(s, "both") == 0)
    {
        return HAL_FLIP_DIRECTION_BOTH;
    }
    return HAL_FLIP_DIRECTION_NONE;
}

const char *isp_pwr_str(HalIspPowerFreq f)
{
    switch (f)
    {
        case HAL_ISP_PWR_FREQ_OFF:
            return "off";
        case HAL_ISP_PWR_FREQ_50HZ:
            return "50";
        case HAL_ISP_PWR_FREQ_60HZ:
            return "60";
        default:
            return "?";
    }
}

bool parse_isp_pwr(const char *s, HalIspPowerFreq *out)
{
    if (strcasecmp(s, "off") == 0)
    {
        *out = HAL_ISP_PWR_FREQ_OFF;
        return true;
    }
    if (strcasecmp(s, "50") == 0)
    {
        *out = HAL_ISP_PWR_FREQ_50HZ;
        return true;
    }
    if (strcasecmp(s, "60") == 0)
    {
        *out = HAL_ISP_PWR_FREQ_60HZ;
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* CLI (same UX as hal_io_cli: raw line, history, tab completion)     */
/* ------------------------------------------------------------------ */

static const char *k_commands[] = {
    "help",
    "quit",
    "exit",
    "streams",
    "versions",
    "media_status",
    "media_start",
    "media_stop",
    "media_restart",
    "media_profile_get",
    "media_profile_list",
    "media_profile_switch",
    "media_profile_json_get",
    "media_profile_backup",
    "media_config_get",
    "media_config_set",
    "media_config_field_get",
    "media_video_add",
    "media_codec_add",
    "media_video_remove",
    "media_codec_remove",
    "media_auto_feed_get",
    "media_auto_feed_set",
    "media_image_rotation",
    "media_image_flip",
    "media_image_zoom",
    "media_image_dewarp",
    "media_image_dis",
    "media_image_eis",
    "media_image_grayscale",
    "media_image_privacy",
    "media_image_privacy_style",
    "media_image_privacy_rect",
    "media_image_dpm_enable",
    "media_image_dpm_test",
    "video_status",
    "video_start",
    "video_stop",
    "video_get_config",
    "video_sensor_info",
    "video_dynamic_resolution",
    "video_dynamic_framerate",
    "video_dynamic_format",
    "video_dynamic_pool",
    "codec_status",
    "codec_start",
    "codec_stop",
    "codec_get_config",
    "codec_dynamic",
    "osd_list",
    "osd_get",
    "osd_clear",
    "osd_remove",
    "osd_enable",
    "osd_add_text",
    "osd_set_text",
    "osd_add_datetime",
    "osd_set_datetime",
    "osd_add_image",
    "osd_set_image",
    "udp_start",
    "udp_stop",
    "udp_select",
    "udp_push",
    "udp_stats",
    "isp_get_image",
    "isp_set_pwr",
    "isp_set_nr",
    "isp_set_wdr",
    "isp_set_awb",
    "isp_awb_list",
    "isp_manual_get",
    "isp_manual_set",
    "isp_exposure_get",
    "isp_exposure_set",
    "isp_af_set",
    "isp_af_get",
    "isp_af_meas",
};
static const size_t k_commands_count = sizeof(k_commands) / sizeof(k_commands[0]);

typedef struct
{
    char items[HISTORY_MAX][MAX_LINE];
    int count;
} CliHistory;

static void history_add(CliHistory *h, const char *line)
{
    if (!h || !line || line[0] == '\0')
    {
        return;
    }
    if (h->count > 0 && std::strcmp(h->items[h->count - 1], line) == 0)
    {
        return;
    }
    if (h->count < HISTORY_MAX)
    {
        std::snprintf(h->items[h->count], MAX_LINE, "%s", line);
        h->count++;
        return;
    }
    for (int i = 1; i < HISTORY_MAX; ++i)
    {
        std::snprintf(h->items[i - 1], MAX_LINE, "%s", h->items[i]);
    }
    std::snprintf(h->items[HISTORY_MAX - 1], MAX_LINE, "%s", line);
}

static void print_prompt_with_cursor(const char *line, size_t cursor)
{
    size_t len = std::strlen(line);
    std::printf("\rmedia> %s", line);
    std::printf("\x1b[K");
    if (cursor < len)
    {
        std::printf("\x1b[%zuD", len - cursor);
    }
    std::fflush(stdout);
}

static void redraw_prompt_after_aux_output(const char *line, size_t cursor)
{
    std::printf("\r");
    print_prompt_with_cursor(line, cursor);
}

static void complete_command(char *line, size_t cap)
{
    if (std::strchr(line, ' ') != NULL)
    {
        return;
    }

    size_t plen = std::strlen(line);
    const char *single = NULL;
    size_t match_count = 0;
    for (size_t i = 0; i < k_commands_count; ++i)
    {
        if (std::strncmp(k_commands[i], line, plen) == 0)
        {
            match_count++;
            single = k_commands[i];
        }
    }

    if (match_count == 1 && single != NULL)
    {
        std::snprintf(line, cap, "%s", single);
        return;
    }

    if (match_count > 1)
    {
        std::printf("\n");
        for (size_t i = 0; i < k_commands_count; ++i)
        {
            if (std::strncmp(k_commands[i], line, plen) == 0)
            {
                std::printf("  %s\n", k_commands[i]);
            }
        }
        redraw_prompt_after_aux_output(line, std::strlen(line));
    }
}

static int read_line_raw(char *line, size_t cap, CliHistory *history)
{
    struct termios oldt;
    struct termios raw;
    if (tcgetattr(STDIN_FILENO, &oldt) != 0)
    {
        return -1;
    }
    raw = oldt;
    raw.c_iflag &= (tcflag_t) ~(IXON | IXOFF);
    raw.c_lflag &= (tcflag_t) ~(ECHO | ICANON | ISIG | IEXTEN);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
    {
        return -1;
    }

    size_t len = 0;
    size_t cursor = 0;
    int hist_pos = (history != nullptr) ? history->count : 0;
    line[0] = '\0';
    print_prompt_with_cursor(line, cursor);

    while (1)
    {
        unsigned char c = 0;
        ssize_t r = read(STDIN_FILENO, &c, 1);
        if (r <= 0)
        {
            (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
            return -1;
        }

        if (c == '\r' || c == '\n')
        {
            break;
        }

        if (c == 0x7F || c == '\b')
        {
            if (cursor > 0 && len > 0)
            {
                std::memmove(&line[cursor - 1], &line[cursor], len - cursor + 1);
                cursor--;
                len--;
                print_prompt_with_cursor(line, cursor);
            }
            continue;
        }

        if (c == '\t')
        {
            complete_command(line, cap);
            len = std::strlen(line);
            cursor = len;
            print_prompt_with_cursor(line, cursor);
            continue;
        }

        if (c == 0x1B)
        {
            unsigned char b1 = 0;
            if (read(STDIN_FILENO, &b1, 1) <= 0)
            {
                continue;
            }
            /* CSI: ESC [ ... final byte 0x40-0x7E (e.g. arrow keys, or [1;5A) */
            if (b1 == '[')
            {
                unsigned char seq[24];
                size_t sn = 0;
                while (sn + 1 < sizeof(seq))
                {
                    if (read(STDIN_FILENO, &seq[sn], 1) <= 0)
                    {
                        sn = 0;
                        break;
                    }
                    sn++;
                    if (seq[sn - 1] >= 0x40 && seq[sn - 1] <= 0x7E)
                    {
                        break;
                    }
                }
                if (sn == 0)
                {
                    continue;
                }
                const unsigned char fin = seq[sn - 1];
                if (history != nullptr)
                {
                    if (fin == 'A')
                    {
                        if (history->count > 0 && hist_pos > 0)
                        {
                            hist_pos--;
                            std::snprintf(line, cap, "%s", history->items[hist_pos]);
                            len = std::strlen(line);
                            cursor = len;
                            print_prompt_with_cursor(line, cursor);
                        }
                    }
                    else if (fin == 'B')
                    {
                        if (history->count > 0 && hist_pos < history->count - 1)
                        {
                            hist_pos++;
                            std::snprintf(line, cap, "%s", history->items[hist_pos]);
                            len = std::strlen(line);
                            cursor = len;
                            print_prompt_with_cursor(line, cursor);
                        }
                        else
                        {
                            hist_pos = history->count;
                            line[0] = '\0';
                            len = 0;
                            cursor = 0;
                            print_prompt_with_cursor(line, cursor);
                        }
                    }
                    else if (fin == 'C' && cursor < len)
                    {
                        cursor++;
                        print_prompt_with_cursor(line, cursor);
                    }
                    else if (fin == 'D' && cursor > 0)
                    {
                        cursor--;
                        print_prompt_with_cursor(line, cursor);
                    }
                }
                continue;
            }
            /* SS3: ESC O A / B / C / D (some terminals) */
            if (b1 == 'O' && history != nullptr)
            {
                unsigned char b2 = 0;
                if (read(STDIN_FILENO, &b2, 1) <= 0)
                {
                    continue;
                }
                if (b2 == 'A' && history->count > 0 && hist_pos > 0)
                {
                    hist_pos--;
                    std::snprintf(line, cap, "%s", history->items[hist_pos]);
                    len = std::strlen(line);
                    cursor = len;
                    print_prompt_with_cursor(line, cursor);
                }
                else if (b2 == 'B')
                {
                    if (history->count > 0 && hist_pos < history->count - 1)
                    {
                        hist_pos++;
                        std::snprintf(line, cap, "%s", history->items[hist_pos]);
                        len = std::strlen(line);
                        cursor = len;
                        print_prompt_with_cursor(line, cursor);
                    }
                    else
                    {
                        hist_pos = history->count;
                        line[0] = '\0';
                        len = 0;
                        cursor = 0;
                        print_prompt_with_cursor(line, cursor);
                    }
                }
                else if (b2 == 'C' && cursor < len)
                {
                    cursor++;
                    print_prompt_with_cursor(line, cursor);
                }
                else if (b2 == 'D' && cursor > 0)
                {
                    cursor--;
                    print_prompt_with_cursor(line, cursor);
                }
                continue;
            }
            continue;
        }

        if (std::isprint(c) != 0 && len + 1 < cap)
        {
            std::memmove(&line[cursor + 1], &line[cursor], len - cursor + 1);
            line[cursor] = (char)c;
            len++;
            cursor++;
            print_prompt_with_cursor(line, cursor);
        }
    }

    (void)tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    std::printf("\n");
    std::fflush(stdout);
    return 0;
}

static void print_help(void)
{
    std::printf(
        "Commands (init/subscribe are only used internally; not exposed here):\n"
        "  help | quit | exit\n"
        "  streams              # print frontend (video) + encoder (codec) indices — use codec idx for UDP\n"
        "  versions             # HAL_MEDIA / VIDEO / CODEC / ISP get_version\n"
        "  media_status | media_start | media_stop | media_restart   # restart = stop then start (recover stalled pipeline)\n"
        "  media_profile_get | media_profile_list | media_profile_switch <name> | media_profile_json_get\n"
        "  media_profile_backup [dir]   # HML JSON backup; omit dir to use init backup_folder_path / JSON default\n"
        "  media_config_get\n"
        "  media_config_set <dotted_path> <bool|int32|uint32|float64|string> <value>\n"
        "      # patch one field of the active profile (e.g. frontend.hailort.use-hailort-service bool true)\n"
        "  media_config_field_get <dotted_path>\n"
        "      # read one scalar field (e.g. frontend.hailort.use-hailort-service)\n"
        "  media_video_add <stream_sid> <w> <h> <fps> [pool] [fmt:nv12|rgb24|gray8|...]\n"
        "  media_codec_add <stream_sid> <h264|h265|mjpeg> <w> <h> <fps> [bitrate_or_quality]\n"
        "  media_video_remove <stream_sid>\n"
        "  media_codec_remove <stream_sid>\n"
        "  media_auto_feed_get | media_auto_feed_set <0|1>\n"
        "  # dynamic_change_image_config (pipeline rotation/flip/zoom/ISP-tuning flags) — state kept in g_image_cfg:\n"
        "  media_image_rotation <0|90|180|270>\n"
        "  media_image_flip <none|h|v|both>\n"
        "  media_image_zoom <off|on> [level 1-31]\n"
        "  media_image_dewarp <0|1> | media_image_dis <0|1> | media_image_eis <0|1> | media_image_grayscale <0|1>\n"
        "  media_throttling                     # query SoC thermal throttling state\n"
        "  motion_set <0|1> [sens 0-4] [thr 0-1] | motion_get | motion_sub   # motion detection\n"
        "  snapshot_list | snapshot [stage]      # multi-stage capture -> /tmp/medialib_snapshots/\n"
        "  isp_wb_set <vidx> <0|1> [r gr gb b] | isp_wb_get <vidx>   # manual white balance\n"
        "  isp_3dnr <vidx> <0|1> <strength>      # temporal NR\n"
        "  isp_ae_stats <vidx>                   # AE histogram + luma grid\n"
        "  isp_hdr_ratios <vidx> <ls>            # HDR exposure ratio (HDR profile only)\n"
        "  enc_roi <cidx> <1|0> <bg_qp 0-15> [x y w h]...   # ROI/smart encoding (H.264+CVBR), coords NORMALIZED 0..1\n"
        "  enc_roi_get <cidx> | enc_force_idr <cidx> | enc_stats <cidx>\n"
        "  media_image_privacy <0|1>              # enable/disable overlay (mutually exclusive with digital zoom)\n"
        "  media_image_privacy_style <blur> <r> <g> <b>   # blur 0=solid color; 2-64=pixelization block size\n"
        "  media_image_privacy_add <id> <x0> <y0> <x1> <y1> [x2 y2 ...]   # add/update region by id, up to 8 verts\n"
        "  media_image_privacy_del <id> | media_image_privacy_list | media_image_privacy_clear\n"
        "  media_image_privacy_rect <x0> <y0> <x1> <y1>   # legacy alias: rect under id 'cli_privacy_rect'\n"
        "  media_image_dpm_enable <1|0>          # enable AI-driven dynamic masking (masked_labels={'person'})\n"
        "  media_image_dpm_test <x> <y> <w> <h> [label]   # upper-layer attach: video cb calls attach_frame_analytics\n"
        "  video_status <idx> | video_start <idx> | video_stop <idx> | video_get_config <idx>\n"
        "  video_sensor_info <idx> [sensor_index]   # FROM_MEDIA only (SensorRegistry); sensor_index 0|1, default 0\n"
        "  video_dynamic_resolution <idx> <w> <h>\n"
        "  video_dynamic_framerate <idx> <fps>\n"
        "  video_dynamic_format <idx> <nv12|nv21|...>\n"
        "  video_dynamic_pool <idx> <n>\n"
        "  # HAL_ISP_OPS (V4L2 ISP; typical sensor path — use frontend video idx, often 0):\n"
        "  isp_get_image <vidx>\n"
        "  isp_set_pwr <vidx> off|50|60    isp_set_nr <vidx> <0-100>    isp_set_wdr <vidx> <0-100>\n"
        "  isp_set_backlight <vidx> <0-100>    # when exposure auto=1 (webserver: /isp/auto_exposure.backlight)\n"
        "  isp_set_awb <vidx> auto|<idx>    isp_awb_list <vidx>   # optional AWB illuminant names from ML profile\n"
        "  isp_manual_get <vidx> | isp_manual_set <vidx> on|off [brightness contrast saturation sharpness]\n"
        "  isp_exposure_get <vidx>\n"
        "  isp_exposure_set <vidx> auto <0|1> [backlight] | isp_exposure_set <vidx> manual <us> <gain>\n"
        "  # AF statistics (Hailo Imaging UG 6.3 AF):\n"
        "  isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]   # PIXEL coords (sensor image; x>=5,y>=2,w*h<=128^3), up to 3 windows\n"
        "  isp_af_get <vidx>\n"
        "  isp_af_meas <vidx>\n"
        "  codec_status <idx> | codec_start <idx> | codec_stop <idx> | codec_get_config <idx>\n"
        "  codec_dynamic <idx> <field> <value>   # bitrate, rc, qp, qp_min, qp_max, gop,\n"
        "                                         # intra_pic_rate, rate_control_gop_length,\n"
        "                                         # bframes, jpeg_quality, intra_qp_delta, fixed_intra_qp, qp_hdr\n"
        "  # HAL_OSD_OPS (encoder-scoped overlays; codec index = encoder row in \"streams\"):\n"
        "  osd_list <cidx>\n"
        "  osd_get <cidx> <id>\n"
        "  osd_clear <cidx>\n"
        "  osd_remove <cidx> <id>\n"
        "  osd_enable <cidx> <id> <0|1>\n"
        "  osd_add_text <cidx> <id> <x> <y> <label_no_spaces> [font_px] [z]\n"
        "  osd_set_text <cidx> <id> <x> <y> <label_no_spaces> [font_px] [z]\n"
        "  osd_add_datetime <cidx> <id> <x> <y> <fmt_no_spaces> [font_px] [z]\n"
        "  osd_set_datetime <cidx> <id> <x> <y> <fmt_no_spaces> [font_px] [z]\n"
        "  osd_add_image <cidx> <id> <x> <y> <w> <h> <path_no_spaces> [z]\n"
        "  osd_set_image <cidx> <id> <x> <y> <w> <h> <path_no_spaces> [z]\n"
        "  # RTP UDP push: codec index = encoder row in \"streams\" (same as udp_select):\n"
        "  udp_start [host] [port] | udp_stop | udp_select|udp_push <codec_idx> [host] [port] | udp_stats  # traffic + I-interval + ts order\n");
    std::fflush(stdout);
}

static HalVideoContext *video_at(int idx)
{
    if (!g_video_list || idx < 0 || static_cast<uint32_t>(idx) >= g_video_count)
    {
        return nullptr;
    }
    return static_cast<HalVideoContext *>(g_video_list[idx]);
}

static HalCodecContext *codec_at(int idx)
{
    if (!g_codec_list || idx < 0 || static_cast<uint32_t>(idx) >= g_codec_count)
    {
        return nullptr;
    }
    return static_cast<HalCodecContext *>(g_codec_list[idx]);
}

static int isp_patch_image(int vidx, const char *tag, const std::function<void(HalIspImageConfig *)> &patch)
{
    HalVideoContext *vc = video_at(vidx);
    if (!vc || !HAL_ISP_OPS.get_current_image_config || !HAL_ISP_OPS.set_image_config)
    {
        std::printf("isp: bad video index or HAL_ISP_OPS not available\n");
        return HAL_ERR_INVALID_ARG;
    }
    HalIspImageConfig cfg{};
    int r = HAL_ISP_OPS.get_current_image_config(vc, &cfg);
    if (r != HAL_OK)
    {
        std::printf("isp_get_current_image_config ret=%d\n", r);
        return r;
    }
    cfg.awb_profile_list = nullptr;
    cfg.awb_profile_count = 0;
    patch(&cfg);
    r = HAL_ISP_OPS.set_image_config(vc, &cfg);
    std::printf("%s ret=%d\n", tag, r);
    return r;
}

static int default_udp_codec_index()
{
    for (uint32_t i = 0; i < g_codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(g_codec_list[i]);
        if (!cc)
        {
            continue;
        }
        if (cc->config.packet_type == HAL_PACKET_TYPE_H264 || cc->config.packet_type == HAL_PACKET_TYPE_H265)
        {
            return static_cast<int>(i);
        }
    }
    return g_codec_count > 0 ? 0 : -1;
}

static bool parse_config_field_type(const char *s, HalConfigFieldType *out)
{
    if (std::strcmp(s, "bool") == 0)        { *out = HAL_CONFIG_FIELD_BOOL; return true; }
    if (std::strcmp(s, "int32") == 0)       { *out = HAL_CONFIG_FIELD_INT32; return true; }
    if (std::strcmp(s, "uint32") == 0)      { *out = HAL_CONFIG_FIELD_UINT32; return true; }
    if (std::strcmp(s, "float64") == 0 ||
        std::strcmp(s, "float") == 0)       { *out = HAL_CONFIG_FIELD_FLOAT64; return true; }
    if (std::strcmp(s, "string") == 0 ||
        std::strcmp(s, "str") == 0)         { *out = HAL_CONFIG_FIELD_STRING; return true; }
    return false;
}

static const char *config_field_type_str(HalConfigFieldType t)
{
    switch (t)
    {
    case HAL_CONFIG_FIELD_BOOL:    return "bool";
    case HAL_CONFIG_FIELD_INT32:   return "int32";
    case HAL_CONFIG_FIELD_UINT32:  return "uint32";
    case HAL_CONFIG_FIELD_FLOAT64: return "float64";
    case HAL_CONFIG_FIELD_STRING:  return "string";
    default:                       return "?";
    }
}

static void motion_event_cb(void *ctx, bool detected, uint64_t frame_id, uint64_t ts, void *user)
{
    std::printf("[motion] %s frame=%llu ts=%llu ctx=%p user=%p\n", detected ? "START" : "STOP",
                (unsigned long long)frame_id, (unsigned long long)ts, ctx, user);
    std::fflush(stdout);
}

static void dispatch_line(int argc, char **av, const char *udp_host_def, uint16_t udp_port_def)
{
    const char *cmd = av[0];

    if (std::strcmp(cmd, "help") == 0)
    {
        print_help();
        return;
    }
    if (std::strcmp(cmd, "streams") == 0)
    {
        (void)refresh_stream_lists();
        print_all_streams();
        return;
    }
    if (std::strcmp(cmd, "versions") == 0)
    {
        const char *mv = HAL_MEDIA_OPS.get_version ? HAL_MEDIA_OPS.get_version() : "(null)";
        const char *vv = HAL_VIDEO_OPS.get_version ? HAL_VIDEO_OPS.get_version() : "(null)";
        const char *cv = HAL_CODEC_OPS.get_version ? HAL_CODEC_OPS.get_version() : "(null)";
        const char *iv = HAL_ISP_OPS.get_version ? HAL_ISP_OPS.get_version() : "(null)";
        const char *ov = HAL_OSD_OPS.get_version ? HAL_OSD_OPS.get_version() : "(null)";
        std::printf("HAL_MEDIA_OPS  %s\n", mv);
        std::printf("HAL_VIDEO_OPS  %s\n", vv);
        std::printf("HAL_CODEC_OPS  %s\n", cv);
        std::printf("HAL_ISP_OPS    %s\n", iv);
        std::printf("HAL_OSD_OPS    %s\n", ov);
        std::fflush(stdout);
        return;
    }

    /* ----------------------------- OSD ----------------------------- */
    if (std::strcmp(cmd, "osd_list") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: osd_list <cidx>\n");
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc || !HAL_OSD_OPS.get_overlays)
        {
            std::printf("osd: bad codec idx or HAL_OSD_OPS not available\n");
            return;
        }
        uint32_t need = 0;
        int r = HAL_OSD_OPS.get_overlays(cc, nullptr, &need);
        if (r != HAL_ERR_INSUFFICIENT_BUFFER && r != HAL_OK)
        {
            std::printf("osd_list: get_overlays(count) ret=%d\n", r);
            return;
        }
        if (need == 0)
        {
            std::printf("osd_list: 0 overlays\n");
            return;
        }
        std::vector<HalOsdOverlay> ovs;
        ovs.resize(need);
        uint32_t cap = need;
        r = HAL_OSD_OPS.get_overlays(cc, ovs.data(), &cap);
        std::printf("osd_list: ret=%d count=%u\n", r, static_cast<unsigned>(cap));
        if (r != HAL_OK)
        {
            return;
        }
        for (uint32_t i = 0; i < cap; i++)
        {
            const HalOsdOverlay &o = ovs[i];
            const HalOsdOverlayBase *b = nullptr;
            if (o.type == HAL_OSD_OVERLAY_IMAGE) b = &o.data.image.base;
            if (o.type == HAL_OSD_OVERLAY_TEXT) b = &o.data.text.base;
            if (o.type == HAL_OSD_OVERLAY_DATETIME) b = &o.data.datetime.text.base;
            if (o.type == HAL_OSD_OVERLAY_CUSTOM) b = &o.data.custom.base;
            if (!b)
            {
                continue;
            }
            const char *t = (o.type == HAL_OSD_OVERLAY_IMAGE) ? "image" :
                            (o.type == HAL_OSD_OVERLAY_TEXT) ? "text" :
                            (o.type == HAL_OSD_OVERLAY_DATETIME) ? "datetime" :
                            (o.type == HAL_OSD_OVERLAY_CUSTOM) ? "custom" : "?";
            std::printf("  [%u] id=%s type=%s en=%u x=%.3f y=%.3f z=%u angle=%u\n",
                        static_cast<unsigned>(i), b->id, t, b->enabled ? 1U : 0U, b->x, b->y,
                        static_cast<unsigned>(b->z_index), static_cast<unsigned>(b->angle));
            if (o.type == HAL_OSD_OVERLAY_TEXT)
            {
                std::printf("       label=%s font_px=%.1f\n", o.data.text.label, o.data.text.font_size);
            }
            if (o.type == HAL_OSD_OVERLAY_DATETIME)
            {
                std::printf("       fmt=%s font_px=%.1f\n", o.data.datetime.datetime_format, o.data.datetime.text.font_size);
            }
            if (o.type == HAL_OSD_OVERLAY_IMAGE)
            {
                std::printf("       w=%.3f h=%.3f path=%s\n", o.data.image.width, o.data.image.height, o.data.image.image_path);
            }
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_get") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: osd_get <cidx> <id>\n");
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc || !HAL_OSD_OPS.get_overlay)
        {
            std::printf("osd: bad codec idx or HAL_OSD_OPS not available\n");
            return;
        }
        HalOsdOverlay o{};
        int r = HAL_OSD_OPS.get_overlay(cc, av[2], &o);
        std::printf("osd_get ret=%d\n", r);
        if (r != HAL_OK) return;
        const HalOsdOverlayBase *b = nullptr;
        if (o.type == HAL_OSD_OVERLAY_IMAGE) b = &o.data.image.base;
        if (o.type == HAL_OSD_OVERLAY_TEXT) b = &o.data.text.base;
        if (o.type == HAL_OSD_OVERLAY_DATETIME) b = &o.data.datetime.text.base;
        if (o.type == HAL_OSD_OVERLAY_CUSTOM) b = &o.data.custom.base;
        if (!b) return;
        std::printf("id=%s type=%u en=%u x=%.3f y=%.3f z=%u\n", b->id, (unsigned)o.type, b->enabled ? 1U : 0U, b->x, b->y,
                    (unsigned)b->z_index);
        if (o.type == HAL_OSD_OVERLAY_TEXT) std::printf("label=%s font_px=%.1f\n", o.data.text.label, o.data.text.font_size);
        if (o.type == HAL_OSD_OVERLAY_DATETIME) std::printf("fmt=%s font_px=%.1f\n", o.data.datetime.datetime_format, o.data.datetime.text.font_size);
        if (o.type == HAL_OSD_OVERLAY_IMAGE) std::printf("w=%.3f h=%.3f path=%s\n", o.data.image.width, o.data.image.height, o.data.image.image_path);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_clear") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: osd_clear <cidx>\n");
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc || !HAL_OSD_OPS.clear_overlays)
        {
            std::printf("osd: bad codec idx or HAL_OSD_OPS not available\n");
            return;
        }
        int r = HAL_OSD_OPS.clear_overlays(cc);
        std::printf("osd_clear ret=%d\n", r);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_remove") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: osd_remove <cidx> <id>\n");
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc || !HAL_OSD_OPS.remove_overlay)
        {
            std::printf("osd: bad codec idx or HAL_OSD_OPS not available\n");
            return;
        }
        int r = HAL_OSD_OPS.remove_overlay(cc, av[2]);
        std::printf("osd_remove ret=%d\n", r);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_enable") == 0)
    {
        if (argc < 4)
        {
            std::printf("usage: osd_enable <cidx> <id> <0|1>\n");
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc || !HAL_OSD_OPS.set_overlay_enabled)
        {
            std::printf("osd: bad codec idx or HAL_OSD_OPS not available\n");
            return;
        }
        bool en = (std::atoi(av[3]) != 0);
        int r = HAL_OSD_OPS.set_overlay_enabled(cc, av[2], en);
        std::printf("osd_enable ret=%d\n", r);
        std::fflush(stdout);
        return;
    }

    auto fill_base = [](HalOsdOverlayBase &b, const char *id, float x, float y, uint32_t z) {
        std::memset(&b, 0, sizeof(b));
        std::snprintf(b.id, sizeof(b.id), "%s", id ? id : "");
        b.enabled = true;
        b.x = x;
        b.y = y;
        b.z_index = z;
        b.angle = 0;
        b.rotation_policy = HAL_OSD_ROTATION_POLICY_TOP_LEFT;
        b.h_align = HAL_OSD_HALIGN_LEFT;
        b.v_align = HAL_OSD_VALIGN_TOP;
    };
    auto fill_text_style = [](HalOsdTextOverlay &t, const char *label, float font_px) {
        std::snprintf(t.label, sizeof(t.label), "%s", label ? label : "");
        t.text_color = HalOsdColor{255, 255, 255, 255};
        t.background_color = HalOsdColor{0, 0, 0, 0};
        /* Match Hailo OSD defaults; empty font path may crash inside MediaLibrary freetype code. */
        std::snprintf(t.font_path, sizeof(t.font_path), "%s", "/usr/share/fonts/ttf/LiberationMono-Regular.ttf");
        t.font_size = font_px;
        t.line_thickness = 0;
        t.shadow_color = HalOsdColor{-1, 0, 0, 0};
        t.shadow_offset_x = 0.0f;
        t.shadow_offset_y = 0.0f;
        t.outline_size = 0;
        t.outline_color = HalOsdColor{-1, 0, 0, 0};
        t.font_weight = HAL_OSD_FONT_WEIGHT_NORMAL;
    };

    if (std::strcmp(cmd, "osd_add_text") == 0 || std::strcmp(cmd, "osd_set_text") == 0)
    {
        if (argc < 6)
        {
            std::printf("usage: %s <cidx> <id> <x> <y> <label_no_spaces> [font_px] [z]\n", cmd);
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc)
        {
            std::printf("osd: bad codec idx\n");
            return;
        }
        float x = (float)std::atof(av[3]);
        float y = (float)std::atof(av[4]);
        float font_px = (argc >= 7) ? (float)std::atof(av[6]) : 32.0f;
        uint32_t z = (argc >= 8) ? (uint32_t)std::atoi(av[7]) : 0U;

        HalOsdTextOverlay t{};
        fill_base(t.base, av[2], x, y, z);
        fill_text_style(t, av[5], font_px);

        int r = HAL_ERR_NOT_IMPLEMENTED;
        if (std::strcmp(cmd, "osd_add_text") == 0 && HAL_OSD_OPS.add_text_overlay) r = HAL_OSD_OPS.add_text_overlay(cc, &t);
        if (std::strcmp(cmd, "osd_set_text") == 0 && HAL_OSD_OPS.set_text_overlay) r = HAL_OSD_OPS.set_text_overlay(cc, &t);
        std::printf("%s ret=%d\n", cmd, r);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_add_datetime") == 0 || std::strcmp(cmd, "osd_set_datetime") == 0)
    {
        if (argc < 6)
        {
            std::printf("usage: %s <cidx> <id> <x> <y> <fmt_no_spaces> [font_px] [z]\n", cmd);
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc)
        {
            std::printf("osd: bad codec idx\n");
            return;
        }
        float x = (float)std::atof(av[3]);
        float y = (float)std::atof(av[4]);
        float font_px = (argc >= 7) ? (float)std::atof(av[6]) : 32.0f;
        uint32_t z = (argc >= 8) ? (uint32_t)std::atoi(av[7]) : 0U;

        /* Accept a friendly format alias: "YYYY-MM-DD" -> "%Y-%m-%d" */
        const char *fmt_in = av[5];
        const char *fmt_norm = (std::strcmp(fmt_in, "YYYY-MM-DD") == 0) ? "%Y-%m-%d" : fmt_in;

        HalOsdDateTimeOverlay d{};
        fill_base(d.text.base, av[2], x, y, z);
        fill_text_style(d.text, "", font_px);
        std::snprintf(d.datetime_format, sizeof(d.datetime_format), "%s", fmt_norm);

        int r = HAL_ERR_NOT_IMPLEMENTED;
        if (std::strcmp(cmd, "osd_add_datetime") == 0 && HAL_OSD_OPS.add_datetime_overlay) r = HAL_OSD_OPS.add_datetime_overlay(cc, &d);
        if (std::strcmp(cmd, "osd_set_datetime") == 0 && HAL_OSD_OPS.set_datetime_overlay) r = HAL_OSD_OPS.set_datetime_overlay(cc, &d);
        std::printf("%s ret=%d\n", cmd, r);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "osd_add_image") == 0 || std::strcmp(cmd, "osd_set_image") == 0)
    {
        if (argc < 8)
        {
            std::printf("usage: %s <cidx> <id> <x> <y> <w> <h> <path_no_spaces> [z]\n", cmd);
            return;
        }
        int idx = std::atoi(av[1]);
        HalCodecContext *cc = codec_at(idx);
        if (!cc)
        {
            std::printf("osd: bad codec idx\n");
            return;
        }
        float x = (float)std::atof(av[3]);
        float y = (float)std::atof(av[4]);
        float w = (float)std::atof(av[5]);
        float h = (float)std::atof(av[6]);
        uint32_t z = (argc >= 9) ? (uint32_t)std::atoi(av[8]) : 0U;

        HalOsdImageOverlay im{};
        fill_base(im.base, av[2], x, y, z);
        im.width = w;
        im.height = h;
        std::snprintf(im.image_path, sizeof(im.image_path), "%s", av[7]);

        int r = HAL_ERR_NOT_IMPLEMENTED;
        if (std::strcmp(cmd, "osd_add_image") == 0 && HAL_OSD_OPS.add_image_overlay) r = HAL_OSD_OPS.add_image_overlay(cc, &im);
        if (std::strcmp(cmd, "osd_set_image") == 0 && HAL_OSD_OPS.set_image_overlay) r = HAL_OSD_OPS.set_image_overlay(cc, &im);
        std::printf("%s ret=%d\n", cmd, r);
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "media_status") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        int st = HAL_MEDIA_OPS.get_status(g_media_ctx);
        std::printf("media_status=%d\n", st);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_start") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        int rc = HAL_MEDIA_OPS.start(g_media_ctx);
        std::printf("media_start ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_stop") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        int rc = HAL_MEDIA_OPS.stop(g_media_ctx);
        std::printf("media_stop ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_restart") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        udp_stop_internal();
        int rs = HAL_MEDIA_OPS.stop(g_media_ctx);
        std::printf("media_restart: stop ret=%d\n", rs);
        int r = HAL_MEDIA_OPS.start(g_media_ctx);
        std::printf("media_restart: start ret=%d\n", r);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_profile_get") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        char *name = nullptr;
        int rc = HAL_MEDIA_OPS.get_current_profile(g_media_ctx, &name);
        std::printf("ret=%d profile=%s\n", rc, (rc == HAL_OK && name) ? name : "(null)");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_profile_list") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        char *names[128];
        uint32_t n = 0;
        int rc = HAL_MEDIA_OPS.get_profile_list(g_media_ctx, names, &n);
        if (rc != HAL_OK)
        {
            std::printf("ret=%d\n", rc);
            std::fflush(stdout);
            return;
        }
        std::printf("profiles: %u\n", static_cast<unsigned>(n));
        for (uint32_t i = 0; i < n && i < 128U; i++)
        {
            std::printf("  [%u] %s\n", static_cast<unsigned>(i), names[i] ? names[i] : "(null)");
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_profile_switch") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_profile_switch <name>\n");
            return;
        }
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        const bool had_udp = (g_udp != nullptr);
        const int prev_idx = g_udp_codec_index;
        udp_stop_internal();
        int rc = HAL_MEDIA_OPS.switch_profile(g_media_ctx, av[1], false);
        std::printf("media_profile_switch ret=%d\n", rc);
        (void)refresh_stream_lists();
        /* Only re-arm RTP after a successful switch; on failure the previous profile may still be active. */
        if (rc == HAL_OK && had_udp && prev_idx >= 0 && g_codec_count > 0)
        {
            int ni = prev_idx < static_cast<int>(g_codec_count) ? prev_idx : 0;
            int u = udp_start_for_index(ni, udp_host_def, udp_port_def);
            std::printf("udp auto-restart ret=%d idx=%d\n", u, ni);
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_profile_json_get") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        const char *j = nullptr;
        int rc = HAL_MEDIA_OPS.get_current_profile_json ? HAL_MEDIA_OPS.get_current_profile_json(g_media_ctx, &j)
                                                        : HAL_ERR_NOT_SUPPORTED;
        std::printf("get_current_profile_json ret=%d\n", rc);
        if (rc == HAL_OK && j)
        {
            std::printf("%s\n", j);
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_profile_backup") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        const char *bp = (argc >= 2 && av[1][0] != '\0') ? av[1] : nullptr;
        int rc = HAL_MEDIA_OPS.backup_current_profile ? HAL_MEDIA_OPS.backup_current_profile(g_media_ctx, bp)
                                                      : HAL_ERR_NOT_SUPPORTED;
        std::printf("backup_current_profile ret=%d path=%s\n", rc, bp ? bp : "(default from init)");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_config_get") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        HalMediaConfig cfg{};
        int rc = HAL_MEDIA_OPS.get_current_config(g_media_ctx, &cfg);
        std::printf("get_current_config ret=%d path=%s json=%s\n", rc,
                    cfg.config_path ? cfg.config_path : "(null)", cfg.config_json ? cfg.config_json : "(null)");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_config_set") == 0)
    {
        if (!g_media_ctx || !HAL_MEDIA_OPS.set_config_field)
        {
            std::printf("no media / set_config_field unsupported\n");
            return;
        }
        if (argc < 4)
        {
            std::printf("usage: media_config_set <dotted_path> <bool|int32|uint32|float64|string> <value>\n"
                        "  e.g. media_config_set frontend.hailort.use-hailort-service bool true\n"
                        "       media_config_set application_settings.optical_zoom.magnification float64 2.0\n");
            return;
        }
        HalConfigFieldType ftype = HAL_CONFIG_FIELD_STRING;
        if (!parse_config_field_type(av[2], &ftype))
        {
            std::printf("invalid type: %s (bool|int32|uint32|float64|string)\n", av[2]);
            return;
        }
        int rc = HAL_MEDIA_OPS.set_config_field(g_media_ctx, av[1], ftype, av[3]);
        std::printf("set_config_field ret=%d path=%s type=%s value=%s\n", rc, av[1], av[2], av[3]);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_config_field_get") == 0)
    {
        if (!g_media_ctx || !HAL_MEDIA_OPS.get_config_field)
        {
            std::printf("no media / get_config_field unsupported\n");
            return;
        }
        if (argc < 2)
        {
            std::printf("usage: media_config_field_get <dotted_path>\n"
                        "  e.g. media_config_field_get frontend.hailort.use-hailort-service\n");
            return;
        }
        HalConfigFieldType ftype = HAL_CONFIG_FIELD_STRING;
        const char *val = nullptr;
        int rc = HAL_MEDIA_OPS.get_config_field(g_media_ctx, av[1], &ftype, &val);
        std::printf("get_config_field ret=%d path=%s type=%s value=%s\n", rc, av[1],
                    config_field_type_str(ftype), val ? val : "(null)");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_video_add") == 0)
    {
        if (argc < 5 || !g_media_ctx)
        {
            std::printf("usage: media_video_add <stream_sid> <w> <h> <fps> [pool] [fmt]\n");
            return;
        }
        HalMediaAddVideoConfig add_cfg{};
        add_cfg.stream_id = av[1];
        add_cfg.video.type = HAL_VIDEO_TYPE_FROM_MEDIA;
        add_cfg.video.width = static_cast<uint32_t>(std::strtoul(av[2], nullptr, 10));
        add_cfg.video.height = static_cast<uint32_t>(std::strtoul(av[3], nullptr, 10));
        add_cfg.video.framerate = static_cast<uint32_t>(std::strtoul(av[4], nullptr, 10));
        add_cfg.video.pool_max_buffers = (argc >= 6) ? static_cast<uint32_t>(std::strtoul(av[5], nullptr, 10)) : 0U;
        add_cfg.video.format = HAL_PIX_FMT_NV12;
        if (argc >= 7)
        {
            HalPixelFormat fmt = HAL_PIX_FMT_NV12;
            if (!parse_pixel_fmt(av[6], &fmt))
            {
                std::printf("invalid pixel format: %s\n", av[6]);
                return;
            }
            add_cfg.video.format = fmt;
        }
        int rc = HAL_MEDIA_OPS.add_video_stream ? HAL_MEDIA_OPS.add_video_stream(g_media_ctx, &add_cfg)
                                                : HAL_ERR_NOT_SUPPORTED;
        std::printf("media_video_add ret=%d\n", rc);
        if (rc == HAL_OK)
        {
            (void)refresh_stream_lists();
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_codec_add") == 0)
    {
        if (argc < 6 || !g_media_ctx)
        {
            std::printf("usage: media_codec_add <stream_sid> <h264|h265|mjpeg> <w> <h> <fps> [bitrate_or_quality]\n");
            return;
        }
        HalMediaAddCodecConfig add_cfg{};
        add_cfg.stream_id = av[1];
        add_cfg.codec.type = HAL_CODEC_TYPE_FROM_MEDIA;
        if (strcasecmp(av[2], "h265") == 0)
        {
            add_cfg.codec.packet_type = HAL_PACKET_TYPE_H265;
            add_cfg.codec.rc_mode = HAL_RC_CVBR;
        }
        else if (strcasecmp(av[2], "mjpeg") == 0 || strcasecmp(av[2], "jpeg") == 0)
        {
            add_cfg.codec.packet_type = HAL_PACKET_TYPE_MJPEG;
        }
        else
        {
            add_cfg.codec.packet_type = HAL_PACKET_TYPE_H264;
            add_cfg.codec.rc_mode = HAL_RC_CVBR;
        }
        add_cfg.codec.width = static_cast<uint32_t>(std::strtoul(av[3], nullptr, 10));
        add_cfg.codec.height = static_cast<uint32_t>(std::strtoul(av[4], nullptr, 10));
        add_cfg.codec.framerate = static_cast<uint32_t>(std::strtoul(av[5], nullptr, 10));
        add_cfg.codec.format = HAL_PIX_FMT_NV12;
        if (argc >= 7)
        {
            if (add_cfg.codec.packet_type == HAL_PACKET_TYPE_MJPEG)
            {
                add_cfg.codec.jpeg_quality = static_cast<uint32_t>(std::strtoul(av[6], nullptr, 10));
            }
            else
            {
                add_cfg.codec.bitrate = static_cast<uint32_t>(std::strtoul(av[6], nullptr, 10));
            }
        }
        int rc = HAL_MEDIA_OPS.add_codec_stream ? HAL_MEDIA_OPS.add_codec_stream(g_media_ctx, &add_cfg)
                                                : HAL_ERR_NOT_SUPPORTED;
        std::printf("media_codec_add ret=%d\n", rc);
        if (rc == HAL_OK)
        {
            (void)refresh_stream_lists();
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_video_remove") == 0)
    {
        if (argc < 2 || !g_media_ctx)
        {
            std::printf("usage: media_video_remove <stream_sid>\n");
            return;
        }
        HalMediaRemoveVideoConfig rem{};
        rem.stream_id = av[1];
        int rc = HAL_MEDIA_OPS.remove_video_stream ? HAL_MEDIA_OPS.remove_video_stream(g_media_ctx, &rem)
                                                    : HAL_ERR_NOT_SUPPORTED;
        std::printf("media_video_remove ret=%d\n", rc);
        if (rc == HAL_OK)
        {
            (void)refresh_stream_lists();
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_codec_remove") == 0)
    {
        if (argc < 2 || !g_media_ctx)
        {
            std::printf("usage: media_codec_remove <stream_sid>\n");
            return;
        }
        HalMediaRemoveCodecConfig rem{};
        rem.stream_id = av[1];
        int rc = HAL_MEDIA_OPS.remove_codec_stream ? HAL_MEDIA_OPS.remove_codec_stream(g_media_ctx, &rem)
                                                    : HAL_ERR_NOT_SUPPORTED;
        std::printf("media_codec_remove ret=%d\n", rc);
        if (rc == HAL_OK)
        {
            (void)refresh_stream_lists();
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_auto_feed_get") == 0)
    {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        bool en = false;
        int rc = HAL_MEDIA_OPS.get_encoder_auto_feed(g_media_ctx, &en);
        std::printf("ret=%d encoder_auto_feed=%d\n", rc, (int)en);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_auto_feed_set") == 0)
    {
        if (argc < 2 || !g_media_ctx)
        {
            std::printf("usage: media_auto_feed_set <0|1>\n");
            return;
        }
        bool en = (std::atoi(av[1]) != 0);
        int rc = HAL_MEDIA_OPS.set_encoder_auto_feed(g_media_ctx, en);
        std::printf("ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_auto_feed_stream_get") == 0)
    {
        if (argc < 2 || !g_media_ctx)
        {
            std::printf("usage: media_auto_feed_stream_get <stream_id>\n");
            return;
        }
        bool en = false;
        int rc = HAL_MEDIA_OPS.get_encoder_auto_feed_for_stream(g_media_ctx, av[1], &en);
        std::printf("ret=%d stream=%s encoder_auto_feed=%d\n", rc, av[1], (int)en);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_auto_feed_stream_set") == 0)
    {
        if (argc < 3 || !g_media_ctx)
        {
            std::printf("usage: media_auto_feed_stream_set <stream_id> <0|1>\n");
            return;
        }
        bool en = (std::atoi(av[2]) != 0);
        int rc = HAL_MEDIA_OPS.set_encoder_auto_feed_for_stream(g_media_ctx, av[1], en);
        std::printf("ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }

    auto apply_image = [](const char *tag) {
        if (!g_media_ctx)
        {
            std::printf("no media\n");
            return;
        }
        int rc = HAL_MEDIA_OPS.dynamic_change_image_config(g_media_ctx, &g_image_cfg);
        std::printf("%s ret=%d\n", tag, rc);
        std::fflush(stdout);
    };

    if (std::strcmp(cmd, "media_image_rotation") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_rotation <0|90|180|270>\n");
            return;
        }
        unsigned deg = (unsigned)std::atoi(av[1]);
        g_image_cfg.rotation_angle = parse_rotation_deg(deg);
        apply_image("media_image_rotation");
        return;
    }
    if (std::strcmp(cmd, "media_image_flip") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_flip <none|h|v|both>\n");
            return;
        }
        g_image_cfg.flip_direction = parse_flip(av[1]);
        apply_image("media_image_flip");
        return;
    }
    if (std::strcmp(cmd, "media_image_zoom") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_zoom <off|on> [level]\n");
            return;
        }
        if (strcasecmp(av[1], "off") == 0)
        {
            g_image_cfg.digital_zoom = false;
            g_image_cfg.digital_zoom_value = 1;
        }
        else
        {
            g_image_cfg.digital_zoom = true;
            g_image_cfg.digital_zoom_value = (argc >= 3) ? std::atoi(av[2]) : 2;
            if (g_image_cfg.digital_zoom_value < 1)
            {
                g_image_cfg.digital_zoom_value = 1;
            }
            if (g_image_cfg.digital_zoom_value > 31)
            {
                g_image_cfg.digital_zoom_value = 5;
            }
            g_image_cfg.privacy_mask = false;
            g_image_cfg.privacy_mask_config.item_count = 0;
            g_image_cfg.privacy_mask_config.items = nullptr;
        }
        apply_image("media_image_zoom");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_privacy <0|1>\n");
            return;
        }
        const int on = std::atoi(av[1]);
        g_image_cfg.privacy_mask = (on != 0);
        g_image_cfg.digital_zoom = false;
        g_image_cfg.digital_zoom_value = 1;
        if (!g_image_cfg.privacy_mask)
        {
            /* Disable: hand the HAL an empty set. g_pm_regions is preserved so a later
             * re-enable re-arms the same regions. */
            g_image_cfg.privacy_mask_config.item_count = 0;
            g_image_cfg.privacy_mask_config.items = nullptr;
        }
        else
        {
            sync_privacy_regions_to_cfg();
        }
        apply_image("media_image_privacy");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_style") == 0)
    {
        if (argc < 5)
        {
            std::printf("usage: media_image_privacy_style <blur_0_to_64> <r> <g> <b>   # blur 0=solid color; 2-64=pixelization block size\n");
            return;
        }
        g_image_cfg.privacy_mask = true;
        g_image_cfg.digital_zoom = false;
        g_image_cfg.digital_zoom_value = 1;
        g_image_cfg.privacy_mask_config.blur_radius = std::atoi(av[1]);
        g_image_cfg.privacy_mask_config.color.r = static_cast<uint8_t>(std::atoi(av[2]));
        g_image_cfg.privacy_mask_config.color.g = static_cast<uint8_t>(std::atoi(av[3]));
        g_image_cfg.privacy_mask_config.color.b = static_cast<uint8_t>(std::atoi(av[4]));
        sync_privacy_regions_to_cfg();
        apply_image("media_image_privacy_style");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_add") == 0)
    {
        /* Add or update a region by id (webserver-style map<id,polygon>).
         * Vertices are normalized [0..1] in display space (post rotation/flip).
         * 4 coords -> axis-aligned rect; 6+ coords -> polygon (up to 8 vertices = 16 coords). */
        if (argc < 6 || (((argc - 2) % 2) != 0))
        {
            std::printf("usage: media_image_privacy_add <id> <x0> <y0> <x1> <y1> [x2 y2 ... up to 8 vertices]\n");
            return;
        }
        const std::string id = av[1];
        const int npairs = (argc - 2) / 2;
        if (npairs < 2 || npairs > 8)
        {
            std::printf("error: vertex count must be 2..8 (got %d)\n", npairs);
            return;
        }
        PrivacyRegion *r = find_privacy_region(id);
        if (!r)
        {
            g_pm_regions.push_back({});
            r = &g_pm_regions.back();
            r->id = id;
        }
        r->enabled = true;
        r->points.clear();
        r->points.reserve(static_cast<size_t>(npairs));
        for (int i = 0; i < npairs; ++i)
        {
            const float x = std::strtof(av[2 + i * 2], nullptr);
            const float y = std::strtof(av[3 + i * 2], nullptr);
            r->points.emplace_back(std::clamp(x, 0.0F, 1.0F), std::clamp(y, 0.0F, 1.0F));
        }
        g_image_cfg.privacy_mask = true;
        g_image_cfg.digital_zoom = false;
        g_image_cfg.digital_zoom_value = 1;
        sync_privacy_regions_to_cfg();
        std::printf("privacy region '%s' set (%d vertices, %zu total)\n", id.c_str(), npairs, g_pm_regions.size());
        apply_image("media_image_privacy_add");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_del") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_privacy_del <id>\n");
            return;
        }
        const std::string id = av[1];
        size_t before = g_pm_regions.size();
        g_pm_regions.erase(std::remove_if(g_pm_regions.begin(), g_pm_regions.end(),
                                          [&](const PrivacyRegion &rr) { return rr.id == id; }),
                           g_pm_regions.end());
        if (g_pm_regions.size() == before)
        {
            std::printf("privacy region '%s' not found\n", id.c_str());
            return;
        }
        sync_privacy_regions_to_cfg();
        std::printf("privacy region '%s' removed (%zu remain)\n", id.c_str(), g_pm_regions.size());
        apply_image("media_image_privacy_del");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_list") == 0)
    {
        std::printf("privacy regions: %zu (overlay %s, blur_radius=%d)\n", g_pm_regions.size(),
                    g_image_cfg.privacy_mask ? "on" : "off", g_image_cfg.privacy_mask_config.blur_radius);
        for (const auto &r : g_pm_regions)
        {
            std::printf("  [%s] %s, %zu vertices:", r.id.c_str(), r.enabled ? "enabled" : "disabled",
                        r.points.size());
            for (const auto &p : r.points)
            {
                std::printf(" (%.3f,%.3f)", p.first, p.second);
            }
            std::printf("\n");
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_clear") == 0)
    {
        g_pm_regions.clear();
        g_image_cfg.privacy_mask_config.item_count = 0;
        g_image_cfg.privacy_mask_config.items = nullptr;
        std::printf("privacy regions cleared\n");
        apply_image("media_image_privacy_clear");
        return;
    }
    if (std::strcmp(cmd, "media_image_privacy_rect") == 0)
    {
        /* Backward-compatible alias: add/update a rect under id "cli_privacy_rect".
         * Coordinates are normalized [0..1] (x0,y0)-(x1,y1). */
        if (argc < 5)
        {
            std::printf("usage: media_image_privacy_rect <x0> <y0> <x1> <y1>   # normalized rect [0..1]\n");
            return;
        }
        const float x0 = std::strtof(av[1], nullptr);
        const float y0 = std::strtof(av[2], nullptr);
        const float x1 = std::strtof(av[3], nullptr);
        const float y1 = std::strtof(av[4], nullptr);
        const std::string rid = "cli_privacy_rect";
        PrivacyRegion *r = find_privacy_region(rid);
        if (!r)
        {
            g_pm_regions.push_back({});
            r = &g_pm_regions.back();
            r->id = rid;
        }
        r->enabled = true;
        r->points.clear();
        r->points.reserve(4);
        r->points.emplace_back(x0, y0);
        r->points.emplace_back(x1, y0);
        r->points.emplace_back(x1, y1);
        r->points.emplace_back(x0, y1);
        g_image_cfg.privacy_mask = true;
        g_image_cfg.digital_zoom = false;
        g_image_cfg.digital_zoom_value = 1;
        sync_privacy_regions_to_cfg();
        apply_image("media_image_privacy_rect");
        return;
    }
    if (std::strcmp(cmd, "media_image_dpm_enable") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_dpm_enable <1|0>\n");
            return;
        }
        const int on = std::atoi(av[1]);
        g_image_cfg.privacy_mask_config.dynamic_enabled = (on != 0);
        if (on)
        {
            /* Enable dynamic masking for the "person" label; dilation 4 px. */
            g_image_cfg.privacy_mask_config.dilation_size = 4;
            g_image_cfg.privacy_mask_config.num_masked_labels = 1;
            std::memset(g_image_cfg.privacy_mask_config.masked_labels, 0,
                        sizeof(g_image_cfg.privacy_mask_config.masked_labels));
            std::snprintf(g_image_cfg.privacy_mask_config.masked_labels[0],
                          sizeof(g_image_cfg.privacy_mask_config.masked_labels[0]), "person");
            /* privacy_mask must be on for apply_hal_privacy_to_profile to write masking. */
            g_image_cfg.privacy_mask = true;
            g_image_cfg.digital_zoom = false;
            g_image_cfg.digital_zoom_value = 1;
        }
        apply_image("media_image_dpm_enable");
        std::printf("dynamic privacy mask %s\n", on ? "enabled (label='person')" : "disabled");
        return;
    }
    if (std::strcmp(cmd, "media_image_dpm_test") == 0)
    {
        if (argc < 5)
        {
            std::printf("usage: media_image_dpm_test <x> <y> <w> <h> [label]   # normalized bbox [0..1]; w=h=0 disables\n");
            return;
        }
        if (!g_media_ctx || !HAL_MEDIA_OPS.attach_frame_analytics)
        {
            std::printf("no media / op unavailable\n");
            return;
        }
        const float x = std::strtof(av[1], nullptr);
        const float y = std::strtof(av[2], nullptr);
        const float w = std::strtof(av[3], nullptr);
        const float h = std::strtof(av[4], nullptr);
        const char *label = (argc >= 6 && av[5][0] != '\0') ? av[5] : "person";
        g_dpm_test.x = x;
        g_dpm_test.y = y;
        g_dpm_test.w = w;
        g_dpm_test.h = h;
        g_dpm_test.enabled = (w > 0.f && h > 0.f);
        std::snprintf(g_dpm_test.label, sizeof(g_dpm_test.label), "%s", label);
        /* Lazily subscribe the dpm callback on every frontend video stream so the upper layer
         * (this test) drives the attach via attach_frame_analytics each frame. */
        if (g_dpm_test.enabled && !g_dpm_subscribed && HAL_VIDEO_OPS.subscribe_stream)
        {
            for (uint32_t i = 0; i < g_video_count; i++)
            {
                auto *vc = static_cast<HalVideoContext *>(g_video_list[i]);
                if (vc && vc->video_name[0] != '\0')
                {
                    (void)HAL_VIDEO_OPS.subscribe_stream(vc, vc->video_name, dpm_video_cb, nullptr);
                }
            }
            g_dpm_subscribed = true;
        }
        std::printf("dpm_test %s (bbox=[%.3f,%.3f,%.3f,%.3f] label='%s')\n", g_dpm_test.enabled ? "on" : "off",
                    g_dpm_test.x, g_dpm_test.y, g_dpm_test.w, g_dpm_test.h, g_dpm_test.label);
        return;
    }
    if (std::strcmp(cmd, "media_image_dewarp") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_dewarp <0|1>\n");
            return;
        }
        g_image_cfg.dewarp = (std::atoi(av[1]) != 0);
        apply_image("media_image_dewarp");
        return;
    }
    if (std::strcmp(cmd, "media_image_dis") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_dis <0|1>\n");
            return;
        }
        g_image_cfg.dis = (std::atoi(av[1]) != 0);
        apply_image("media_image_dis");
        return;
    }
    if (std::strcmp(cmd, "media_image_eis") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_eis <0|1>\n");
            return;
        }
        g_image_cfg.eis = (std::atoi(av[1]) != 0);
        apply_image("media_image_eis");
        return;
    }
    if (std::strcmp(cmd, "media_image_grayscale") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: media_image_grayscale <0|1>\n");
            return;
        }
        g_image_cfg.grayscale = (std::atoi(av[1]) != 0);
        apply_image("media_image_grayscale");
        return;
    }

    /* ------------------- Encoder smart control (M1) ------------------- */

    if (std::strcmp(cmd, "enc_roi") == 0)
    {
        if (argc < 4)
        {
            std::printf("usage: enc_roi <cidx> <1|0> <bg_qp_delta 0-15> [x y w h]...   # up to %d ROIs, coords NORMALIZED 0..1\n",
                        HAL_CODEC_ROI_MAX);
            std::fflush(stdout);
            return;
        }
        int cidx = std::atoi(av[1]);
        auto *cc = static_cast<HalCodecContext *>(
            (g_codec_list && cidx >= 0 && static_cast<uint32_t>(cidx) < g_codec_count) ? g_codec_list[cidx] : nullptr);
        if (!cc || !HAL_CODEC_OPS.set_roi_config)
        {
            std::printf("enc_roi: bad codec index or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalCodecRoiConfig roi{};
        roi.enabled = (std::atoi(av[2]) != 0);
        roi.background_qp_delta = std::atoi(av[3]);
        int argi = 4;
        while (argi + 3 < argc && roi.roi_count < HAL_CODEC_ROI_MAX)
        {
            roi.rois[roi.roi_count].x = std::atof(av[argi]);
            roi.rois[roi.roi_count].y = std::atof(av[argi + 1]);
            roi.rois[roi.roi_count].w = std::atof(av[argi + 2]);
            roi.rois[roi.roi_count].h = std::atof(av[argi + 3]);
            roi.roi_count++;
            argi += 4;
        }
        int rc = HAL_CODEC_OPS.set_roi_config(cc, &roi);
        std::printf("enc_roi ret=%d (%s) enabled=%d bg_qp=%d rois=%u\n", rc, hal_error_to_string((HalErrorCode)rc),
                    (int)roi.enabled, roi.background_qp_delta, roi.roi_count);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "enc_roi_get") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: enc_roi_get <cidx>\n");
            std::fflush(stdout);
            return;
        }
        int cidx = std::atoi(av[1]);
        auto *cc = static_cast<HalCodecContext *>(
            (g_codec_list && cidx >= 0 && static_cast<uint32_t>(cidx) < g_codec_count) ? g_codec_list[cidx] : nullptr);
        if (!cc || !HAL_CODEC_OPS.get_roi_config)
        {
            std::printf("enc_roi_get: bad codec index or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalCodecRoiConfig roi{};
        int rc = HAL_CODEC_OPS.get_roi_config(cc, &roi);
        std::printf("enc_roi_get ret=%d enabled=%d bg_qp=%d rois=%u\n", rc, (int)roi.enabled,
                    roi.background_qp_delta, roi.roi_count);
        for (uint32_t i = 0; i < roi.roi_count; ++i)
        {
            std::printf("  roi[%u] x=%.3f y=%.3f w=%.3f h=%.3f\n", i, roi.rois[i].x, roi.rois[i].y, roi.rois[i].w,
                        roi.rois[i].h);
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "enc_force_idr") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: enc_force_idr <cidx>\n");
            std::fflush(stdout);
            return;
        }
        int cidx = std::atoi(av[1]);
        auto *cc = static_cast<HalCodecContext *>(
            (g_codec_list && cidx >= 0 && static_cast<uint32_t>(cidx) < g_codec_count) ? g_codec_list[cidx] : nullptr);
        if (!cc || !HAL_CODEC_OPS.force_idr)
        {
            std::printf("enc_force_idr: bad codec index or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        int rc = HAL_CODEC_OPS.force_idr(cc);
        std::printf("enc_force_idr ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "enc_stats") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: enc_stats <cidx>\n");
            std::fflush(stdout);
            return;
        }
        int cidx = std::atoi(av[1]);
        auto *cc = static_cast<HalCodecContext *>(
            (g_codec_list && cidx >= 0 && static_cast<uint32_t>(cidx) < g_codec_count) ? g_codec_list[cidx] : nullptr);
        if (!cc || !HAL_CODEC_OPS.get_stream_stats)
        {
            std::printf("enc_stats: bad codec index or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalCodecStreamStats st{};
        int rc = HAL_CODEC_OPS.get_stream_stats(cc, &st);
        std::printf("enc_stats ret=%d fps=%.2f bitrate=%u kbps window=%us\n", rc, st.fps, st.bitrate_kbps,
                    st.monitor_period_s);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "media_throttling") == 0)
    {
        if (!g_media_ctx || !HAL_MEDIA_OPS.get_throttling_state)
        {
            std::printf("media_throttling: media not initialized or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalThrottlingState st = HAL_THROTTLING_UNINITIALIZED;
        int rc = HAL_MEDIA_OPS.get_throttling_state(g_media_ctx, &st);
        const char *names[] = {"UNINITIALIZED", "FULL_PERFORMANCE", "COOLING", "S0", "S1", "S2", "S3", "S4"};
        const char *nm = (st >= 0 && st <= HAL_THROTTLING_S4) ? names[st] : "?";
        std::printf("media_throttling ret=%d state=%s (%d)\n", rc, nm, (int)st);
        std::fflush(stdout);
        return;
    }

#define NEED_VIDX()                                                                                                                \
    if (argc < 2)                                                                                                                  \
    {                                                                                                                              \
        std::printf("missing video index\n");                                                                                      \
        return;                                                                                                                    \
    }                                                                                                                              \
    int vidx = std::atoi(av[1]);                                                                                                   \
    HalVideoContext *vc = video_at(vidx);                                                                                          \
    if (!vc)                                                                                                                       \
    {                                                                                                                              \
        std::printf("bad video index %d\n", vidx);                                                                                 \
        return;                                                                                                                    \
    }

    if (std::strcmp(cmd, "video_status") == 0)
    {
        NEED_VIDX();
        HalStatus s = HAL_VIDEO_OPS.get_status(vc);
        std::printf("video[%d] status=%s (%d)\n", vidx, status_str(s), (int)s);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_start") == 0)
    {
        NEED_VIDX();
        int rc = HAL_VIDEO_OPS.start(vc);
        std::printf("video_start ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_stop") == 0)
    {
        NEED_VIDX();
        int rc = HAL_VIDEO_OPS.stop(vc);
        std::printf("video_stop ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_get_config") == 0)
    {
        NEED_VIDX();
        HalVideoConfig cfg{};
        int rc = HAL_VIDEO_OPS.get_current_config(vc, &cfg);
        std::printf("video_get_config ret=%d type=%d %ux%u @%u fmt=%s pool=%u name=%s\n", rc, (int)cfg.type, cfg.width,
                    cfg.height, cfg.framerate, hal_pixel_format_to_string(cfg.format), cfg.pool_max_buffers, vc->video_name);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_sensor_info") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: video_sensor_info <idx> [sensor_index]\n");
            std::printf("  FROM_MEDIA only: HAL_VIDEO_OPS.get_sensor_module_info -> SensorRegistry (model / I2C / V4L2 fourcc).\n");
            std::printf("  sensor_index: 0 = primary sensor, 1 = secondary (dual-sensor); default 0.\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *v = video_at(i);
        if (!v)
        {
            std::printf("bad video index\n");
            return;
        }
        uint32_t sensor_index = 0;
        if (argc >= 3)
        {
            sensor_index = static_cast<uint32_t>(std::strtoul(av[2], nullptr, 10));
        }
        if (!HAL_VIDEO_OPS.get_sensor_module_info)
        {
            std::printf("video_sensor_info: get_sensor_module_info not implemented\n");
            std::fflush(stdout);
            return;
        }
        HalVideoSensorModuleInfo mod{};
        const int rc = HAL_VIDEO_OPS.get_sensor_module_info(v, sensor_index, &mod);
        std::printf("video_sensor_info[%d] sensor_index=%u ret=%d (%s) valid_fields=0x%x\n", i,
                    static_cast<unsigned>(sensor_index), rc,
                    (rc == HAL_OK) ? "ok" : hal_error_to_string(static_cast<HalErrorCode>(rc)),
                    static_cast<unsigned>(mod.valid_fields));
        if (rc == HAL_OK)
        {
            if ((mod.valid_fields & HAL_VIDEO_SENSOR_INFO_VALID_MODEL_NAME) != 0U)
            {
                std::printf("  model=%s\n", mod.sensor_model_name);
            }
            if ((mod.valid_fields & HAL_VIDEO_SENSOR_INFO_VALID_I2C) != 0U)
            {
                std::printf("  i2c bus=%d addr=%s\n", static_cast<int>(mod.i2c_bus), mod.i2c_address);
            }
            if ((mod.valid_fields & HAL_VIDEO_SENSOR_INFO_VALID_PIXEL_FORMAT) != 0U)
            {
                std::printf("  sensor_pixel_format (V4L2 fourcc int)=%d\n", static_cast<int>(mod.sensor_pixel_format));
            }
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_dynamic_resolution") == 0)
    {
        if (argc < 4)
        {
            std::printf("usage: video_dynamic_resolution <idx> <w> <h>\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *v = video_at(i);
        if (!v)
        {
            std::printf("bad video index\n");
            return;
        }
        uint32_t w = (uint32_t)std::strtoul(av[2], nullptr, 10);
        uint32_t h = (uint32_t)std::strtoul(av[3], nullptr, 10);
        int rc = HAL_VIDEO_OPS.dynamic_change_resolution(v, w, h);
        std::printf("video_dynamic_resolution ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_dynamic_framerate") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: video_dynamic_framerate <idx> <fps>\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *v = video_at(i);
        if (!v)
        {
            std::printf("bad video index\n");
            return;
        }
        uint32_t fps = (uint32_t)std::strtoul(av[2], nullptr, 10);
        int rc = HAL_VIDEO_OPS.dynamic_change_framerate(v, fps);
        std::printf("video_dynamic_framerate ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_dynamic_format") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: video_dynamic_format <idx> <nv12|...>\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *v = video_at(i);
        if (!v)
        {
            std::printf("bad video index\n");
            return;
        }
        HalPixelFormat fmt{};
        if (!parse_pixel_fmt(av[2], &fmt))
        {
            std::printf("unknown format\n");
            return;
        }
        int rc = HAL_VIDEO_OPS.dynamic_change_format(v, fmt);
        std::printf("video_dynamic_format ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "video_dynamic_pool") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: video_dynamic_pool <idx> <n>\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *v = video_at(i);
        if (!v)
        {
            std::printf("bad video index\n");
            return;
        }
        uint32_t n = (uint32_t)std::strtoul(av[2], nullptr, 10);
        int rc = HAL_VIDEO_OPS.dynamic_change_pool_max_buffers(v, n);
        std::printf("video_dynamic_pool ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }

#undef NEED_VIDX

#define NEED_CIDX()                                                                                                                \
    if (argc < 2)                                                                                                                  \
    {                                                                                                                              \
        std::printf("missing codec index\n");                                                                                      \
        return;                                                                                                                    \
    }                                                                                                                              \
    int cidx = std::atoi(av[1]);                                                                                                   \
    HalCodecContext *cc = codec_at(cidx);                                                                                          \
    if (!cc)                                                                                                                       \
    {                                                                                                                              \
        std::printf("bad codec index %d\n", cidx);                                                                                 \
        return;                                                                                                                    \
    }

    if (std::strcmp(cmd, "codec_status") == 0)
    {
        NEED_CIDX();
        int st = HAL_CODEC_OPS.get_status(cc);
        std::printf("codec[%d] status=%d\n", cidx, st);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "codec_start") == 0)
    {
        NEED_CIDX();
        int rc = HAL_CODEC_OPS.start(cc);
        std::printf("codec_start ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "codec_stop") == 0)
    {
        NEED_CIDX();
        int rc = HAL_CODEC_OPS.stop(cc);
        std::printf("codec_stop ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "codec_get_config") == 0)
    {
        NEED_CIDX();
        HalCodecConfig cfg{};
        int rc = HAL_CODEC_OPS.get_current_config(cc, &cfg);
        std::printf(
            "codec_get_config ret=%d type=%d pkt=%s %ux%u @%u rc=%s bitrate=%u qp=%u gop=%u intra=%u rc_gop=%u bframes=%u "
            "jpeg_q=%u\n",
            rc, (int)cfg.type, packet_type_str(cfg.packet_type), cfg.width, cfg.height, cfg.framerate, rc_str(cfg.rc_mode),
            cfg.bitrate, cfg.qp, cfg.gop_size, cfg.intra_pic_rate, cfg.rate_control_gop_length, cfg.b_frames,
            cfg.jpeg_quality);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "codec_dynamic") == 0)
    {
        if (argc < 4)
        {
            std::printf("usage: codec_dynamic <idx> <field> <value>\n");
            return;
        }
        int i = std::atoi(av[1]);
        auto *c = codec_at(i);
        if (!c)
        {
            std::printf("bad codec index\n");
            return;
        }
        HalCodecConfig cfg{};
        if (HAL_CODEC_OPS.get_current_config(c, &cfg) != HAL_OK)
        {
            std::printf("get_current_config failed\n");
            return;
        }
        const char *field = av[2];
        const char *val = av[3];
        if (std::strcmp(field, "bitrate") == 0)
        {
            cfg.bitrate = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "rc") == 0)
        {
            HalRateControlMode m{};
            if (!parse_rc_mode(val, &m))
            {
                std::printf("bad rc mode\n");
                return;
            }
            cfg.rc_mode = m;
        }
        else if (std::strcmp(field, "qp") == 0)
        {
            cfg.qp = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "qp_min") == 0)
        {
            cfg.qp_min = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "qp_max") == 0)
        {
            cfg.qp_max = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "gop") == 0)
        {
            /* Legacy semantics: 'gop' used to drive all GOP-related knobs together.
             * Keep it so users don't accidentally create an invalid (gop_length, intra_pic_rate) pair. */
            const uint32_t v = (uint32_t)std::strtoul(val, nullptr, 10);
            cfg.gop_size = v;
            cfg.intra_pic_rate = v;
            cfg.rate_control_gop_length = v;
        }
        else if (std::strcmp(field, "intra_pic_rate") == 0 || std::strcmp(field, "intra") == 0)
        {
            cfg.intra_pic_rate = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "rate_control_gop_length") == 0 || std::strcmp(field, "gop_length") == 0 ||
                 std::strcmp(field, "rc_gop") == 0)
        {
            cfg.rate_control_gop_length = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "bframes") == 0)
        {
            cfg.b_frames = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "jpeg_quality") == 0)
        {
            cfg.jpeg_quality = (uint32_t)std::strtoul(val, nullptr, 10);
        }
        else if (std::strcmp(field, "intra_qp_delta") == 0)
        {
            cfg.intra_qp_delta = std::atoi(val);
        }
        else if (std::strcmp(field, "fixed_intra_qp") == 0)
        {
            cfg.fixed_intra_qp = std::atoi(val);
        }
        else if (std::strcmp(field, "qp_hdr") == 0)
        {
            cfg.qp_hdr = std::atoi(val);
        }
        else
        {
            std::printf("unknown field\n");
            return;
        }
        int rc = HAL_CODEC_OPS.dynamic_change_config(c, &cfg);
        std::printf("codec_dynamic ret=%d\n", rc);
        (void)refresh_stream_lists();
        std::fflush(stdout);
        return;
    }

#undef NEED_CIDX

    /* ------------------- ISP M2 additions ------------------- */

    if (std::strcmp(cmd, "isp_wb_set") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_wb_set <vidx> <0|1> [r gr gb b]   # manual WB gains, 1.0=neutral\n");
            std::fflush(stdout);
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.set_wb_config)
        {
            std::printf("isp_wb_set: bad vidx or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalIspWbConfig wb{};
        wb.manual_state = (std::atoi(av[2]) != 0);
        wb.r_gain = wb.gr_gain = wb.gb_gain = wb.b_gain = 1.0f;
        if (argc >= 7)
        {
            wb.r_gain = std::atof(av[3]);
            wb.gr_gain = std::atof(av[4]);
            wb.gb_gain = std::atof(av[5]);
            wb.b_gain = std::atof(av[6]);
        }
        const int rc = HAL_ISP_OPS.set_wb_config(vc, &wb);
        std::printf("isp_wb_set ret=%d manual=%d r=%.3f gr=%.3f gb=%.3f b=%.3f\n", rc, (int)wb.manual_state,
                    wb.r_gain, wb.gr_gain, wb.gb_gain, wb.b_gain);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_wb_get") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_wb_get <vidx>\n");
            std::fflush(stdout);
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.get_current_wb_config)
        {
            std::printf("isp_wb_get: bad vidx or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalIspWbConfig wb{};
        const int rc = HAL_ISP_OPS.get_current_wb_config(vc, &wb);
        std::printf("isp_wb_get ret=%d manual=%d r=%.3f gr=%.3f gb=%.3f b=%.3f\n", rc, (int)wb.manual_state,
                    wb.r_gain, wb.gr_gain, wb.gb_gain, wb.b_gain);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_3dnr") == 0)
    {
        if (argc < 4)
        {
            std::printf("usage: isp_3dnr <vidx> <0|1> <strength 0-100>\n");
            std::fflush(stdout);
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.set_3dnr_config)
        {
            std::printf("isp_3dnr: bad vidx or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalIspNr3dConfig nr{};
        nr.enabled = (std::atoi(av[2]) != 0);
        nr.strength = std::atoi(av[3]);
        const int rc = HAL_ISP_OPS.set_3dnr_config(vc, &nr);
        std::printf("isp_3dnr ret=%d (%s) enabled=%d strength=%d\n", rc,
                    hal_error_to_string((HalErrorCode)rc), (int)nr.enabled, nr.strength);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_ae_stats") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_ae_stats <vidx>\n");
            std::fflush(stdout);
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.get_ae_stats)
        {
            std::printf("isp_ae_stats: bad vidx or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        static HalIspAeStats st; /* large: keep off the stack */
        const int rc = HAL_ISP_OPS.get_ae_stats(vc, &st);
        if (rc != HAL_OK)
        {
            std::printf("isp_ae_stats ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
            std::fflush(stdout);
            return;
        }
        uint64_t total = 0;
        uint32_t peak = 0;
        int peak_bin = -1;
        if (st.hist_valid)
        {
            for (int i = 0; i < 256; ++i)
            {
                total += st.hist[i];
                if (st.hist[i] > peak)
                {
                    peak = st.hist[i];
                    peak_bin = i;
                }
            }
        }
        std::printf("isp_ae_stats ret=%d hist_valid=%d total=%llu peak_bin=%d luma_valid=%d luma[12]=%u\n", rc,
                    (int)st.hist_valid, (unsigned long long)total, peak_bin, (int)st.luma_valid,
                    st.luma_valid ? st.luma[12] : 0U);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_hdr_ratios") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_hdr_ratios <vidx> <ls_ratio>   # HDR profile only\n");
            std::fflush(stdout);
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.set_hdr_ratios)
        {
            std::printf("isp_hdr_ratios: bad vidx or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        const int rc = HAL_ISP_OPS.set_hdr_ratios(vc, std::atof(av[2]), 0.0f);
        std::printf("isp_hdr_ratios ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        std::fflush(stdout);
        return;
    }

    /* ------------------- motion / snapshot (M2) ------------------- */

    if (std::strcmp(cmd, "motion_set") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: motion_set <0|1> [sens 0-4] [threshold 0-1]   # sensitivity: 0=lowest..4=highest\n");
            std::fflush(stdout);
            return;
        }
        if (!g_media_ctx || !HAL_MEDIA_OPS.set_motion_config)
        {
            std::printf("motion_set: media not initialized or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalMotionConfig mc{};
        mc.enabled = (std::atoi(av[1]) != 0);
        mc.sensitivity = (argc >= 3) ? static_cast<HalMotionSensitivity>(std::atoi(av[2]))
                                     : HAL_MOTION_SENSITIVITY_MEDIUM;
        mc.threshold = (argc >= 4) ? std::atof(av[3]) : 0.05f;
        const int rc = HAL_MEDIA_OPS.set_motion_config(g_media_ctx, &mc);
        std::printf("motion_set ret=%d (%s) enabled=%d sens=%d thr=%.3f\n", rc,
                    hal_error_to_string((HalErrorCode)rc), (int)mc.enabled, (int)mc.sensitivity, mc.threshold);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "motion_get") == 0)
    {
        if (!g_media_ctx || !HAL_MEDIA_OPS.get_motion_config)
        {
            std::printf("motion_get: media not initialized or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        HalMotionConfig mc{};
        const int rc = HAL_MEDIA_OPS.get_motion_config(g_media_ctx, &mc);
        std::printf("motion_get ret=%d enabled=%d sens=%d thr=%.3f roi=%d,%d %dx%d\n", rc, (int)mc.enabled,
                    (int)mc.sensitivity, mc.threshold, mc.roi_x, mc.roi_y, mc.roi_w, mc.roi_h);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "motion_sub") == 0)
    {
        if (!g_media_ctx || !HAL_MEDIA_OPS.subscribe_motion)
        {
            std::printf("motion_sub: media not initialized or op unavailable\n");
            std::fflush(stdout);
            return;
        }
        const int rc = HAL_MEDIA_OPS.subscribe_motion(g_media_ctx, motion_event_cb, nullptr);
        std::printf("motion_sub ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "snapshot_list") == 0)
    {
        if (!HAL_VIDEO_OPS.list_snapshot_stages)
        {
            std::printf("snapshot_list: op unavailable\n");
            std::fflush(stdout);
            return;
        }
        static char stages[2048];
        const int rc = HAL_VIDEO_OPS.list_snapshot_stages(nullptr, stages, sizeof(stages));
        std::printf("snapshot_list ret=%d stages:\n%s\n", rc, stages);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "snapshot") == 0)
    {
        if (!HAL_VIDEO_OPS.request_snapshot)
        {
            std::printf("snapshot: op unavailable\n");
            std::fflush(stdout);
            return;
        }
        const char *stage = (argc >= 2) ? av[1] : nullptr;
        const int rc = HAL_VIDEO_OPS.request_snapshot(nullptr, stage);
        std::printf("snapshot ret=%d (%s) stage=%s   # files land under /tmp/medialib_snapshots/<ts>/\n", rc,
                    hal_error_to_string((HalErrorCode)rc), stage ? stage : "(all)");
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "isp_get_image") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_get_image <vidx>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.get_current_image_config)
        {
            std::printf("isp: bad vidx or ops\n");
            return;
        }
        HalIspImageConfig cfg{};
        int r = HAL_ISP_OPS.get_current_image_config(vc, &cfg);
        if (r != HAL_OK)
        {
            std::printf("isp_get_image ret=%d\n", r);
            return;
        }
        std::printf("isp_get_image ret=%d pwr=%s nr=%d wdr=%d awb_idx=%d\n", r, isp_pwr_str(cfg.pwr_freq),
                    cfg.noise_reduction, cfg.wdr_value, cfg.awb_idx);
        std::printf("  manual: state=%d b=%d c=%d s=%d sh=%d\n", (int)cfg.manual_config.manual_state,
                    cfg.manual_config.brightness, cfg.manual_config.contrast, cfg.manual_config.saturation,
                    cfg.manual_config.sharpness);
        std::printf("  exposure: auto=%d backlight=%d time_us=%d gain=%d\n", (int)cfg.exposure_config.auto_exposure,
                    cfg.exposure_config.backlight, cfg.exposure_config.exposure_time_us, cfg.exposure_config.gain);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_set_pwr") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_set_pwr <vidx> off|50|60\n");
            return;
        }
        HalIspPowerFreq pf{};
        if (!parse_isp_pwr(av[2], &pf))
        {
            std::printf("bad pwr\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        (void)isp_patch_image(vidx, "isp_set_pwr", [pf](HalIspImageConfig *c) { c->pwr_freq = pf; });
        return;
    }
    if (std::strcmp(cmd, "isp_set_nr") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_set_nr <vidx> <0-100>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        const int nr = std::atoi(av[2]);
        (void)isp_patch_image(vidx, "isp_set_nr", [nr](HalIspImageConfig *c) { c->noise_reduction = nr; });
        return;
    }
    if (std::strcmp(cmd, "isp_set_wdr") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_set_wdr <vidx> <0-100>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        const int w = std::atoi(av[2]);
        (void)isp_patch_image(vidx, "isp_set_wdr", [w](HalIspImageConfig *c) { c->wdr_value = w; });
        return;
    }
    if (std::strcmp(cmd, "isp_set_backlight") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_set_backlight <vidx> <0-100>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        const int bl = std::atoi(av[2]);
        (void)isp_patch_image(vidx, "isp_set_backlight", [bl](HalIspImageConfig *c) {
            c->exposure_config.auto_exposure = true;
            c->exposure_config.backlight = bl;
        });
        return;
    }
    if (std::strcmp(cmd, "isp_set_awb") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_set_awb <vidx> auto|<illuminant_idx>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        if (strcasecmp(av[2], "auto") == 0)
        {
            (void)isp_patch_image(vidx, "isp_set_awb", [](HalIspImageConfig *c) { c->awb_idx = -1; });
        }
        else
        {
            const int ai = std::atoi(av[2]);
            (void)isp_patch_image(vidx, "isp_set_awb", [ai](HalIspImageConfig *c) { c->awb_idx = ai; });
        }
        return;
    }
    if (std::strcmp(cmd, "isp_awb_list") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_awb_list <vidx>\n");
            return;
        }
        HalVideoContext *vc = video_at(std::atoi(av[1]));
        if (!vc || !HAL_ISP_OPS.get_current_image_config)
        {
            std::printf("isp: bad vidx or ops\n");
            return;
        }
        HalIspImageConfig cfg{};
        const int r = HAL_ISP_OPS.get_current_image_config(vc, &cfg);
        std::printf("isp_awb_list ret=%d count=%u current_awb_idx=%d\n", r, static_cast<unsigned>(cfg.awb_profile_count),
                    cfg.awb_idx);
        if (r == HAL_OK && cfg.awb_profile_count > 0U && cfg.awb_profile_list != nullptr)
        {
            for (uint32_t i = 0; i < cfg.awb_profile_count; ++i)
            {
                const char *nm = cfg.awb_profile_list[i] ? cfg.awb_profile_list[i] : "";
                std::printf("  [%u] %s\n", static_cast<unsigned>(i), nm);
            }
        }
        else if (r == HAL_OK)
        {
            std::printf("  (no illuminant name list in profile — check iq_settings.automatic_algorithms.aw_drv4)\n");
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_manual_get") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_manual_get <vidx>\n");
            return;
        }
        HalVideoContext *vc = video_at(std::atoi(av[1]));
        if (!vc || !HAL_ISP_OPS.get_current_manual_config)
        {
            std::printf("isp: bad vidx\n");
            return;
        }
        HalIspManualConfig m{};
        int r = HAL_ISP_OPS.get_current_manual_config(vc, &m);
        std::printf("isp_manual_get ret=%d state=%d b=%d c=%d s=%d sh=%d\n", r, (int)m.manual_state, m.brightness,
                    m.contrast, m.saturation, m.sharpness);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_manual_set") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_manual_set <vidx> on|off [brightness contrast saturation sharpness]\n");
            return;
        }
        HalVideoContext *vc = video_at(std::atoi(av[1]));
        if (!vc || !HAL_ISP_OPS.get_current_manual_config || !HAL_ISP_OPS.set_manual_config)
        {
            std::printf("isp: bad vidx\n");
            return;
        }
        HalIspManualConfig m{};
        (void)HAL_ISP_OPS.get_current_manual_config(vc, &m);
        if (strcasecmp(av[2], "on") == 0)
        {
            m.manual_state = true;
        }
        else if (strcasecmp(av[2], "off") == 0)
        {
            m.manual_state = false;
        }
        else
        {
            std::printf("use on|off\n");
            return;
        }
        if (argc >= 7)
        {
            m.brightness = std::atoi(av[3]);
            m.contrast = std::atoi(av[4]);
            m.saturation = std::atoi(av[5]);
            m.sharpness = std::atoi(av[6]);
        }
        int r = HAL_ISP_OPS.set_manual_config(vc, &m);
        std::printf("isp_manual_set ret=%d\n", r);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_exposure_get") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_exposure_get <vidx>\n");
            return;
        }
        HalVideoContext *vc = video_at(std::atoi(av[1]));
        if (!vc || !HAL_ISP_OPS.get_current_exposure_config)
        {
            std::printf("isp: bad vidx\n");
            return;
        }
        HalIspExposureConfig e{};
        int r = HAL_ISP_OPS.get_current_exposure_config(vc, &e);
        std::printf("isp_exposure_get ret=%d auto=%d backlight=%d time_us=%d gain=%d\n", r, (int)e.auto_exposure,
                    e.backlight, e.exposure_time_us, e.gain);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "isp_exposure_set") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_exposure_set <vidx> auto <0|1> [backlight] | isp_exposure_set <vidx> manual <us> "
                        "<gain>\n");
            return;
        }
        HalVideoContext *vc = video_at(std::atoi(av[1]));
        if (!vc || !HAL_ISP_OPS.get_current_exposure_config || !HAL_ISP_OPS.set_exposure_config)
        {
            std::printf("isp: bad vidx\n");
            return;
        }
        HalIspExposureConfig e{};
        (void)HAL_ISP_OPS.get_current_exposure_config(vc, &e);
        if (strcasecmp(av[2], "auto") == 0)
        {
            if (argc < 4)
            {
                std::printf("usage: isp_exposure_set <vidx> auto <0|1> [backlight]\n");
                return;
            }
            e.auto_exposure = (std::atoi(av[3]) != 0);
            if (e.auto_exposure && argc >= 5)
            {
                e.backlight = std::atoi(av[4]);
            }
        }
        else if (strcasecmp(av[2], "manual") == 0)
        {
            if (argc < 5)
            {
                std::printf("usage: isp_exposure_set <vidx> manual <time_us> <gain>\n");
                return;
            }
            e.auto_exposure = false;
            e.exposure_time_us = std::atoi(av[3]);
            e.gain = std::atoi(av[4]);
        }
        else
        {
            std::printf("use auto or manual\n");
            return;
        }
        int r = HAL_ISP_OPS.set_exposure_config(vc, &e);
        std::printf("isp_exposure_set ret=%d\n", r);
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "isp_af_set") == 0)
    {
        if (argc < 3)
        {
            std::printf("usage: isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]\n");
            std::printf("       AF windows are PIXEL coords on the sensor image (x>=5, y>=2, w*h<=128^3)\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.set_af_windows_config)
        {
            std::printf("isp_af_set: bad video index or HAL_ISP_OPS.set_af_windows_config not available\n");
            return;
        }

        HalIspAfWindowsConfig cfg{};
        cfg.enabled = (std::atoi(av[2]) != 0);
        if (!cfg.enabled)
        {
            cfg.window_count = 0;
            const int r = HAL_ISP_OPS.set_af_windows_config(vc, &cfg);
            std::printf("isp_af_set ret=%d\n", r);
            std::fflush(stdout);
            return;
        }

        if (argc < 7)
        {
            std::printf("usage: isp_af_set <vidx> <0|1> <x1> <y1> <w1> <h1> [x2 y2 w2 h2] [x3 y3 w3 h3]\n");
            return;
        }

        const int remain = argc - 3;
        const int win_n = std::clamp(remain / 4, 1, static_cast<int>(HAL_ISP_AF_MAX_WINDOWS));
        cfg.window_count = static_cast<uint32_t>(win_n);
        for (int i = 0; i < win_n; ++i)
        {
            const int base = 3 + i * 4;
            cfg.windows[i].x = std::atoi(av[base + 0]);
            cfg.windows[i].y = std::atoi(av[base + 1]);
            cfg.windows[i].w = std::atoi(av[base + 2]);
            cfg.windows[i].h = std::atoi(av[base + 3]);
        }

        const int r = HAL_ISP_OPS.set_af_windows_config(vc, &cfg);
        std::printf("isp_af_set ret=%d enabled=1 windows=%u\n", r, static_cast<unsigned>(cfg.window_count));
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "isp_af_get") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_af_get <vidx>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.get_af_windows_config)
        {
            std::printf("isp_af_get: bad video index or HAL_ISP_OPS.get_af_windows_config not available\n");
            return;
        }
        HalIspAfWindowsConfig cfg{};
        const int r = HAL_ISP_OPS.get_af_windows_config(vc, &cfg);
        std::printf("isp_af_get ret=%d enabled=%d windows=%u\n", r, static_cast<int>(cfg.enabled),
                    static_cast<unsigned>(cfg.window_count));
        for (uint32_t i = 0; i < cfg.window_count && i < HAL_ISP_AF_MAX_WINDOWS; ++i)
        {
            std::printf("  win%u: x=%d y=%d w=%d h=%d\n", static_cast<unsigned>(i + 1), cfg.windows[i].x,
                        cfg.windows[i].y, cfg.windows[i].w, cfg.windows[i].h);
        }
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "isp_af_meas") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: isp_af_meas <vidx>\n");
            return;
        }
        const int vidx = std::atoi(av[1]);
        HalVideoContext *vc = video_at(vidx);
        if (!vc || !HAL_ISP_OPS.get_af_measurement)
        {
            std::printf("isp_af_meas: bad video index or HAL_ISP_OPS.get_af_measurement not available\n");
            return;
        }
        HalIspAfMeasurement m{};
        const int r = HAL_ISP_OPS.get_af_measurement(vc, &m);
        std::printf("isp_af_meas ret=%d windows=%u sum=[%u,%u,%u] luma=[%u,%u,%u]\n", r,
                    static_cast<unsigned>(m.window_count), m.sum[0], m.sum[1], m.sum[2], m.luma[0], m.luma[1],
                    m.luma[2]);
        std::fflush(stdout);
        return;
    }

    if (std::strcmp(cmd, "udp_stop") == 0)
    {
        udp_stop_internal();
        std::printf("udp stopped\n");
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_stats") == 0)
    {
        if (!g_udp)
        {
            std::printf("udp not running\n");
            return;
        }
        uint64_t b = g_udp->bytes_sent();
        uint64_t p = g_udp->packets_sent();
        CodecUdpStats st{};
        {
            std::lock_guard<std::mutex> lock(g_codec_udp_stats_mu);
            st = g_codec_udp_stats;
        }
        std::printf("udp codec_idx=%d bytes=%llu packets=%llu\n", g_udp_codec_index,
                    static_cast<unsigned long long>(b), static_cast<unsigned long long>(p));
        std::printf(
            "  encoded_pkts=%llu ts_strict_increase_ok=%s ts_regress_or_equal=%llu\n",
            static_cast<unsigned long long>(st.pkt_count), (st.ts_not_strictly_increasing == 0) ? "yes" : "no",
            static_cast<unsigned long long>(st.ts_not_strictly_increasing));
        if (st.idr_interval_count > 0)
        {
            const uint64_t avg_pkts = st.idr_interval_sum_pkts / st.idr_interval_count;
            const uint64_t avg_ns = st.idr_interval_sum_ns / st.idr_interval_count;
            std::printf(
                "  I_frame: idr_units=%llu intervals=%llu pkt[min avg max]=[%llu %llu %llu] "
                "ns[min avg max]=[%llu %llu %llu]\n",
                static_cast<unsigned long long>(st.idr_seen), static_cast<unsigned long long>(st.idr_interval_count),
                static_cast<unsigned long long>(st.idr_interval_min_pkts),
                static_cast<unsigned long long>(avg_pkts),
                static_cast<unsigned long long>(st.idr_interval_max_pkts),
                static_cast<unsigned long long>(st.idr_interval_min_ns),
                static_cast<unsigned long long>(avg_ns),
                static_cast<unsigned long long>(st.idr_interval_max_ns));
        }
        else
        {
            std::printf("  I_frame: idr_units=%llu (need >=2 IDR for interval stats; H264/H265 only)\n",
                        static_cast<unsigned long long>(st.idr_seen));
        }
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_start") == 0)
    {
        const char *h = (argc >= 2) ? av[1] : udp_host_def;
        uint16_t port = (argc >= 3) ? (uint16_t)std::atoi(av[2]) : udp_port_def;
        int idx = default_udp_codec_index();
        if (idx < 0)
        {
            std::printf("no codec context\n");
            return;
        }
        int rc = udp_start_for_index(idx, h, port);
        std::printf("udp_start ret=%d\n", rc);
        std::fflush(stdout);
        return;
    }
    if (std::strcmp(cmd, "udp_select") == 0 || std::strcmp(cmd, "udp_push") == 0)
    {
        if (argc < 2)
        {
            std::printf("usage: udp_select|udp_push <codec_idx> [host] [port]   # encoder index from \"streams\"\n");
            return;
        }
        int idx = std::atoi(av[1]);
        const char *h = (argc >= 3) ? av[2] : udp_host_def;
        uint16_t port = (argc >= 4) ? (uint16_t)std::atoi(av[3]) : udp_port_def;
        int rc = udp_start_for_index(idx, h, port);
        std::printf("%s ret=%d\n", cmd, rc);
        std::fflush(stdout);
        return;
    }

    std::printf("unknown command (try help)\n");
    std::fflush(stdout);
}

} // namespace

int main(int argc, char **argv)
{
    const char *json_path = nullptr;   // nullptr => HAL uses its compiled-in default config
    const char *backup_dir = nullptr;
    const char *udp_host = "127.0.0.1";
    uint16_t udp_port = 5004;

    auto endswith = [](const char *s, const char *suf) {
        size_t ls = std::strlen(s), lf = std::strlen(suf);
        return ls >= lf && std::strcmp(s + ls - lf, suf) == 0;
    };

    std::vector<std::string> positionals;
    for (int i = 1; i < argc; i++)
    {
        std::string a = argv[i];
        if (a == "--default") { json_path = nullptr; continue; }  // force embedded default
        if (a == "--backup" && i + 1 < argc) { backup_dir = argv[++i]; continue; }
        if (a == "-h" || a == "--help")
        {
            std::fprintf(stderr,
                         "Usage: %s [medialib_json_path] [--backup <dir>] [--default] [udp_host] [udp_port]\n"
                         "  medialib_json_path  optional; when omitted (or --default given), the HAL uses its\n"
                         "                      compiled-in default media config (5 Basic profiles, 3 streams each).\n"
                         "  --backup <dir>      default output dir for media_profile_backup\n",
                         argv[0]);
            return 0;
        }
        positionals.push_back(a);
    }

    /* First positional is the json path only if it looks like one; otherwise it's udp_host. */
    size_t pidx = 0;
    if (!positionals.empty())
    {
        const char *first = positionals[0].c_str();
        if (std::strchr(first, '/') != nullptr || endswith(first, ".json"))
        {
            json_path = first;
            pidx = 1;
        }
    }
    if (pidx < positionals.size()) { udp_host = positionals[pidx++].c_str(); }
    if (pidx < positionals.size()) { udp_port = static_cast<uint16_t>(std::atoi(positionals[pidx++].c_str())); }

    install_signal_handlers();

    HalMediaConfig mcfg{};
    mcfg.config_path = json_path;
    mcfg.config_json = nullptr;
    mcfg.image_config = {};
    mcfg.backup_folder_path = backup_dir;

    std::printf("init media: config=%s\n", json_path ? json_path : "(embedded default)");
    int rc = HAL_MEDIA_OPS.init(&mcfg, &g_media_ctx);
    if (rc != HAL_OK || !g_media_ctx)
    {
        log_err("HAL_MEDIA_OPS.init failed: %d", rc);
        return 1;
    }

    rc = HAL_MEDIA_OPS.start(g_media_ctx);
    if (rc != HAL_OK)
    {
        log_err("HAL_MEDIA_OPS.start failed: %d", rc);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    if (refresh_stream_lists() != HAL_OK)
    {
        log_err("refresh_stream_lists failed");
        (void)HAL_MEDIA_OPS.stop(g_media_ctx);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    std::printf("=== test_media_all_func (config=%s) ===\n", json_path ? json_path : "(embedded default)");
    print_all_streams();

    const int def_idx = default_udp_codec_index();
    if (def_idx >= 0)
    {
        int ur = udp_start_for_index(def_idx, udp_host, udp_port);
        if (ur != HAL_OK)
        {
            std::printf("warning: default UDP start failed ret=%d (use udp_start manually)\n", ur);
        }
    }
    else
    {
        std::printf("warning: no codec contexts; UDP not started\n");
    }

    print_help();

    CliHistory history{};
    char line[MAX_LINE];
    while (!g_shutdown.load(std::memory_order_acquire))
    {
        if (g_signal_pending != 0)
        {
            g_signal_pending = 0;
            std::printf("signal: exit\n");
            break;
        }

        if (read_line_raw(line, sizeof(line), &history) != 0)
        {
            break;
        }
        if (line[0] == '\0')
        {
            continue;
        }
        history_add(&history, line);

        char *argvv[32] = {0};
        int argcv = 0;
        char *tok = std::strtok(line, " ");
        while (tok && argcv < 32)
        {
            argvv[argcv++] = tok;
            tok = std::strtok(nullptr, " ");
        }
        if (argcv == 0)
        {
            continue;
        }
        if (std::strcmp(argvv[0], "quit") == 0 || std::strcmp(argvv[0], "exit") == 0)
        {
            break;
        }
        dispatch_line(argcv, argvv, udp_host, udp_port);
    }

    udp_stop_internal();
    HAL_LOG_INFO("media stop / deinit");
    (void)HAL_MEDIA_OPS.stop(g_media_ctx);
    (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
    g_media_ctx = nullptr;
    return 0;
}
