/**
 * @file video_test_sub_v2.cpp
 * @brief HAL v2 equivalent of hal/test_example/video_test_sub: push-mode multi-resize frontend + HW encoders + UDP/RTP out.
 *
 * Uses HAL_VIDEO_TYPE_CSI (standalone MediaLibraryFrontend, no HalMedia). Vendor
 * `frontend_config_example.json` omits `input_video.source_type`; medialib defaults it
 * to V4L2SRC, so the bin still pulls from the camera stack and HAL only sees callbacks.
 * Our bundled JSON sets `source_type` + `source` + `stream_id` explicitly for clarity.
 *
 * Flow (aligned with V1 video_test_sub intent):
 *   - One CSI video context, frontend JSON lists multiple output resolutions (stream_id sink0..).
 *   - Per stream: subscribe_stream → in callback: HAL_CODEC_OPS.input_frame → HAL_VIDEO_OPS.release_frame.
 *   - Per stream: standalone HAL_CODEC_TYPE_HW encoder + subscribe → HalUdpStream (RTP Annex B) for remote inference.
 *
 * Usage:
 *   hal-video-test-sub-v2 -f /path/to/frontend.json [-t seconds] [-H host] [-P base_port] [-c h264|h265]
 *                         [-q udp_queue_depth] [-m max_payload]
 *
 * Defaults: -t 10, -H 127.0.0.1, -P 5004 (sink i uses UDP port base_port + i*2, RTP Annex B),
 *           -q 512 (avoid dropping access units), -m 1200 (MTU-safe payload).
 * Example JSON (V4L2 + 3 scales): examples/video_test_sub_v2/data/frontend_v4l2_multires_3streams.json
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"
#include "hailo15_hal_video_codec_ext.h"
#include "media/hal_codec.h"
#include "media/hal_video.h"
#include "media/hal_video_internal.h"

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <csignal>
#include <thread>
#include <unistd.h>
#include <vector>

#include <strings.h>

namespace
{

struct Lane
{
    void *video_ctx{nullptr};
    void *codec_ctx{nullptr};
    std::unique_ptr<HalUdpStream> udp;
    std::string stream_id;
    uint32_t width{0};
    uint32_t height{0};
    uint32_t fps{30};
    std::string encoder_sid;
    uint64_t pkt_count{0};
    uint64_t pkt_logged{0};
    uint64_t sps_seen{0};
    uint64_t pps_seen{0};
    uint64_t idr_seen{0};
};

static std::atomic<bool> g_run{true};

static void on_sig(int)
{
    g_run = false;
}

static bool has_annexb_startcode(const uint8_t *p, size_t n, size_t *sc_len_out);
static void log_packet_header(const char *tag, const uint8_t *p, size_t n);
static void scan_h264_annexb_nal_types(const uint8_t *p, size_t n, uint64_t *sps, uint64_t *pps, uint64_t *idr);

static bool read_file(const char *path, std::string *out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
    {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    *out = ss.str();
    return !out->empty();
}

static bool parse_frontend_lanes(const char *json_path, std::vector<Lane> *lanes_out)
{
    std::string raw;
    if (!read_file(json_path, &raw))
    {
        HAL_LOG_ERROR("video_test_sub_v2: cannot read frontend json '%s'", json_path ? json_path : "(null)");
        return false;
    }
    try
    {
        auto j = nlohmann::json::parse(raw, nullptr, false);
        if (j.is_discarded() || !j.contains("application_input_streams"))
        {
            HAL_LOG_ERROR("video_test_sub_v2: invalid frontend json");
            return false;
        }
        const auto &res = j["application_input_streams"]["resolutions"];
        if (!res.is_array())
        {
            HAL_LOG_ERROR("video_test_sub_v2: resolutions is not an array");
            return false;
        }
        lanes_out->clear();
        for (size_t i = 0; i < res.size(); ++i)
        {
            const auto &r = res[i];
            Lane L{};
            L.stream_id = r.value("stream_id", std::string("sink") + std::to_string(i));
            L.width = r.value("width", 0u);
            L.height = r.value("height", 0u);
            L.fps = r.value("framerate", 30u);
            if (L.width == 0u || L.height == 0u)
            {
                HAL_LOG_ERROR("video_test_sub_v2: bad resolution entry at index %zu", i);
                return false;
            }
            L.encoder_sid = std::string("hal_sub_") + std::to_string(i);
            lanes_out->push_back(std::move(L));
        }
        return !lanes_out->empty();
    }
    catch (const std::exception &e)
    {
        HAL_LOG_ERROR("video_test_sub_v2: json parse failed: %s", e.what());
        return false;
    }
}

static void codec_udp_cb(void *codec_ctx, HalPacketBuffer *pkt, void *userdata)
{
    auto *lane = static_cast<Lane *>(userdata);
    if (!codec_ctx || !pkt || !lane)
    {
        if (pkt)
            (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
        return;
    }
    if (!lane->udp || !lane->udp->ok() || !pkt->data || pkt->size == 0)
    {
        (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
        return;
    }
    lane->pkt_count++;

    size_t sc_len = 0;
    const bool annexb = has_annexb_startcode(pkt->data, pkt->size, &sc_len);
    if (lane->pkt_logged < 3)
    {
        HAL_LOG_INFO("enc[%s]: pkt size=%u ts=%llu seq=%u annexb=%d sc_len=%zu",
                     lane->stream_id.c_str(), static_cast<unsigned>(pkt->size),
                     static_cast<unsigned long long>(pkt->timestamp_ns), static_cast<unsigned>(pkt->sequence),
                     annexb ? 1 : 0, sc_len);
        log_packet_header("enc pkt", pkt->data, pkt->size);
        lane->pkt_logged++;
    }
    if (annexb && pkt->size > sc_len)
    {
        scan_h264_annexb_nal_types(pkt->data, pkt->size, &lane->sps_seen, &lane->pps_seen, &lane->idr_seen);
        if ((lane->pkt_count % 120) == 0)
        {
            HAL_LOG_INFO("enc[%s]: stats pkts=%llu sps=%llu pps=%llu idr=%llu",
                         lane->stream_id.c_str(), static_cast<unsigned long long>(lane->pkt_count),
                         static_cast<unsigned long long>(lane->sps_seen),
                         static_cast<unsigned long long>(lane->pps_seen),
                         static_cast<unsigned long long>(lane->idr_seen));
        }
    }
    std::vector<uint8_t> copy(pkt->data, pkt->data + pkt->size);
    lane->udp->push_annex_b(std::move(copy), pkt->timestamp_ns);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

static bool has_annexb_startcode(const uint8_t *p, size_t n, size_t *sc_len_out)
{
    if (!p || n < 3)
        return false;
    if (n >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
    {
        if (sc_len_out)
            *sc_len_out = 4;
        return true;
    }
    if (p[0] == 0 && p[1] == 0 && p[2] == 1)
    {
        if (sc_len_out)
            *sc_len_out = 3;
        return true;
    }
    return false;
}

static void log_packet_header(const char *tag, const uint8_t *p, size_t n)
{
    if (!p || n == 0)
        return;
    const size_t k = (n < 16) ? n : 16;
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (size_t i = 0; i < k; ++i)
    {
        ss << std::setw(2) << static_cast<unsigned>(p[i]);
        if (i + 1 != k)
            ss << " ";
    }
    HAL_LOG_INFO("%s head[%zu]=%s", tag ? tag : "pkt", k, ss.str().c_str());
}

static void scan_h264_annexb_nal_types(const uint8_t *p, size_t n, uint64_t *sps, uint64_t *pps, uint64_t *idr)
{
    if (!p || n < 4)
        return;
    size_t i = 0;
    while (i + 3 < n)
    {
        size_t sc = 0;
        if (i + 4 < n && p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 0 && p[i + 3] == 1)
            sc = 4;
        else if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1)
            sc = 3;
        if (sc == 0)
        {
            i++;
            continue;
        }
        i += sc;
        if (i >= n)
            break;
        const uint8_t nal_type = static_cast<uint8_t>(p[i] & 0x1F);
        if (nal_type == 7 && sps)
            (*sps)++;
        else if (nal_type == 8 && pps)
            (*pps)++;
        else if (nal_type == 5 && idr)
            (*idr)++;
    }
}

static void video_frame_cb(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *lane = static_cast<Lane *>(userdata);
    if (!lane || !frame || !lane->codec_ctx)
    {
        if (frame && video_ctx)
        {
            (void)HAL_VIDEO_OPS.release_frame(video_ctx, frame);
        }
        return;
    }
    if (!g_run.load())
    {
        (void)HAL_VIDEO_OPS.release_frame(video_ctx, frame);
        return;
    }
    const int ir = HAL_CODEC_OPS.input_frame(lane->codec_ctx, frame);
    if (ir != HAL_OK)
    {
        HAL_LOG_ERROR("input_frame failed stream=%s rc=%d", lane->stream_id.c_str(), ir);
    }
    (void)HAL_VIDEO_OPS.release_frame(video_ctx, frame);
}

} // namespace

int main(int argc, char **argv)
{
    int duration_sec = 10;
    const char *frontend_path = nullptr;
    const char *udp_host = "127.0.0.1";
    int base_port = 5004;
    size_t udp_queue_depth = 512;
    size_t udp_max_payload = 1200;
    HalPacketType pkt_type = HAL_PACKET_TYPE_H264;
    HalUdpStreamMode udp_mode = HalUdpStreamMode::RtpH264AnnexB;
    uint8_t rtp_pt = 96;

    int opt = 0;
    while ((opt = getopt(argc, argv, "t:f:H:P:c:q:m:h")) != -1)
    {
        switch (opt)
        {
            case 't':
                duration_sec = std::atoi(optarg);
                if (duration_sec <= 0)
                {
                    duration_sec = 10;
                }
                break;
            case 'f':
                frontend_path = optarg;
                break;
            case 'H':
                udp_host = optarg;
                break;
            case 'P':
                base_port = std::atoi(optarg);
                break;
            case 'c':
                if (strcasecmp(optarg, "h265") == 0 || strcasecmp(optarg, "hevc") == 0)
                {
                    pkt_type = HAL_PACKET_TYPE_H265;
                    udp_mode = HalUdpStreamMode::RtpH265AnnexB;
                    rtp_pt = 97;
                }
                else
                {
                    pkt_type = HAL_PACKET_TYPE_H264;
                    udp_mode = HalUdpStreamMode::RtpH264AnnexB;
                    rtp_pt = 96;
                }
                break;
            case 'q':
                udp_queue_depth = static_cast<size_t>(std::strtoul(optarg, nullptr, 10));
                if (udp_queue_depth < 8)
                {
                    udp_queue_depth = 8;
                }
                break;
            case 'm':
                udp_max_payload = static_cast<size_t>(std::strtoul(optarg, nullptr, 10));
                if (udp_max_payload < 200)
                {
                    udp_max_payload = 200;
                }
                break;
            case 'h':
            default:
                std::fprintf(stderr,
                             "Usage: %s [-t sec] [-f frontend.json] [-H host] [-P base_port] [-c h264|h265] [-q udp_queue] [-m mtu_payload]\n",
                             argv[0]);
                return 2;
        }
    }

    if (!frontend_path)
    {
        std::fprintf(stderr, "%s: require -f /path/to/frontend.json (see examples/video_test_sub_v2/data/)\n",
                     argv[0]);
        return 2;
    }

    std::vector<Lane> lanes;
    if (!parse_frontend_lanes(frontend_path, &lanes))
    {
        return 1;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    Hailo15HalVideoPrivExt vext{};
    vext.csi_pipeline_mode = HAILO15_CSI_PIPELINE_FRONTEND_ONLY;
    vext.frontend_config_path = frontend_path;

    HalVideoConfig vcfg{};
    vcfg.type = HAL_VIDEO_TYPE_CSI;
    vcfg.path = nullptr;
    vcfg.media_ptr = nullptr;
    vcfg.width = lanes[0].width;
    vcfg.height = lanes[0].height;
    vcfg.framerate = lanes[0].fps;
    vcfg.format = HAL_PIX_FMT_NV12;
    vcfg.pool_max_buffers = 0;
    vcfg.priv = &vext;

    void *video_ctx = nullptr;
    int rc = HAL_VIDEO_OPS.init(&vcfg, &video_ctx);
    if (rc != HAL_OK || !video_ctx)
    {
        HAL_LOG_ERROR("HAL_VIDEO_OPS.init failed: %d", rc);
        return 1;
    }

    std::vector<Hailo15HalCodecPrivExt> codec_privs(lanes.size());
    for (size_t i = 0; i < lanes.size(); ++i)
    {
        lanes[i].video_ctx = video_ctx;

        Hailo15HalCodecPrivExt &cext = codec_privs[i];
        std::memset(&cext, 0, sizeof(cext));
        cext.encoder_stream_id = lanes[i].encoder_sid.c_str();
        cext.config_path = nullptr;
        cext.config_json = nullptr;
        cext.osd_config_path = nullptr;
        cext.osd_config_json = nullptr;

        HalCodecConfig cc{};
        cc.type = HAL_CODEC_TYPE_HW;
        cc.packet_type = pkt_type;
        cc.path = nullptr;
        cc.media_ptr = nullptr;
        cc.width = lanes[i].width;
        cc.height = lanes[i].height;
        cc.format = HAL_PIX_FMT_NV12;
        cc.framerate = lanes[i].fps;
        cc.rc_mode = HAL_RC_CBR;
        cc.bitrate = 8u * 1000u * 1000u;
        /* Smaller GOP improves decoder recovery on packet loss. */
        cc.gop_size = 30u;
        cc.b_frames = 0u;
        cc.priv = &cext;

        void *codec_ctx = nullptr;
        rc = HAL_CODEC_OPS.init(&cc, &codec_ctx);
        if (rc != HAL_OK || !codec_ctx)
        {
            HAL_LOG_ERROR("HAL_CODEC_OPS.init failed lane %zu: %d", i, rc);
            goto fail_partial_codecs;
        }
        lanes[i].codec_ctx = codec_ctx;

        HalUdpStreamConfig ucfg{};
        ucfg.host = udp_host;
        ucfg.port = static_cast<uint16_t>(base_port + static_cast<int>(i) * 2);
        ucfg.mode = udp_mode;
        ucfg.rtp_payload_type = rtp_pt;
        ucfg.queue_depth = udp_queue_depth;
        ucfg.max_payload = udp_max_payload;
        lanes[i].udp = std::make_unique<HalUdpStream>(ucfg);
        if (!lanes[i].udp->ok())
        {
            HAL_LOG_ERROR("HalUdpStream init failed lane %zu host=%s port=%u", i, udp_host ? udp_host : "",
                          static_cast<unsigned>(ucfg.port));
            (void)HAL_CODEC_OPS.deinit(lanes[i].codec_ctx);
            lanes[i].codec_ctx = nullptr;
            goto fail_partial_codecs;
        }

        rc = HAL_CODEC_OPS.subscribe(codec_ctx, codec_udp_cb, &lanes[i]);
        if (rc != HAL_OK)
        {
            HAL_LOG_ERROR("HAL_CODEC_OPS.subscribe failed lane %zu: %d", i, rc);
            lanes[i].udp->shutdown();
            lanes[i].udp.reset();
            (void)HAL_CODEC_OPS.deinit(lanes[i].codec_ctx);
            lanes[i].codec_ctx = nullptr;
            goto fail_partial_codecs;
        }

        rc = HAL_CODEC_OPS.start(codec_ctx);
        if (rc != HAL_OK)
        {
            HAL_LOG_ERROR("HAL_CODEC_OPS.start failed lane %zu: %d", i, rc);
            (void)HAL_CODEC_OPS.unsubscribe(lanes[i].codec_ctx, codec_udp_cb);
            lanes[i].udp->shutdown();
            lanes[i].udp.reset();
            (void)HAL_CODEC_OPS.deinit(lanes[i].codec_ctx);
            lanes[i].codec_ctx = nullptr;
            goto fail_partial_codecs;
        }

        HAL_LOG_INFO("lane %zu: stream=%s %ux%u -> RTP %s:%u (%s)", i, lanes[i].stream_id.c_str(),
                     static_cast<unsigned>(lanes[i].width), static_cast<unsigned>(lanes[i].height), udp_host,
                     static_cast<unsigned>(ucfg.port), pkt_type == HAL_PACKET_TYPE_H265 ? "H265" : "H264");
    }

    for (size_t i = 0; i < lanes.size(); ++i)
    {
        rc = HAL_VIDEO_OPS.subscribe_stream(video_ctx, lanes[i].stream_id.c_str(), video_frame_cb, &lanes[i]);
        if (rc != HAL_OK)
        {
            HAL_LOG_ERROR("subscribe_stream(%s) failed: %d", lanes[i].stream_id.c_str(), rc);
            goto fail_cleanup;
        }
    }

    rc = HAL_VIDEO_OPS.start(video_ctx);
    if (rc != HAL_OK)
    {
        HAL_LOG_ERROR("HAL_VIDEO_OPS.start failed: %d", rc);
        goto fail_cleanup;
    }

    HAL_LOG_INFO("running %zu streams for %d s (Ctrl+C to stop)...", lanes.size(), duration_sec);
    for (int t = 0; g_run.load() && t < duration_sec; ++t)
    {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    (void)HAL_VIDEO_OPS.stop(video_ctx);
    for (size_t i = 0; i < lanes.size(); ++i)
    {
        (void)HAL_VIDEO_OPS.unsubscribe_stream(video_ctx, lanes[i].stream_id.c_str());
    }

    for (size_t i = 0; i < lanes.size(); ++i)
    {
        if (lanes[i].codec_ctx)
        {
            (void)HAL_CODEC_OPS.stop(lanes[i].codec_ctx);
            (void)HAL_CODEC_OPS.unsubscribe(lanes[i].codec_ctx, codec_udp_cb);
            (void)HAL_CODEC_OPS.deinit(lanes[i].codec_ctx);
            lanes[i].codec_ctx = nullptr;
        }
        if (lanes[i].udp)
        {
            lanes[i].udp->shutdown();
            lanes[i].udp.reset();
        }
    }

    (void)HAL_VIDEO_OPS.deinit(video_ctx);
    HAL_LOG_INFO("done. ffplay udp://%s:%d ... (per-stream ports +2)", udp_host, base_port);
    return 0;

fail_cleanup:
    (void)HAL_VIDEO_OPS.stop(video_ctx);
    for (auto &L : lanes)
    {
        (void)HAL_VIDEO_OPS.unsubscribe_stream(video_ctx, L.stream_id.c_str());
    }
    for (auto &L : lanes)
    {
        if (L.codec_ctx)
        {
            (void)HAL_CODEC_OPS.stop(L.codec_ctx);
            (void)HAL_CODEC_OPS.unsubscribe(L.codec_ctx, codec_udp_cb);
            (void)HAL_CODEC_OPS.deinit(L.codec_ctx);
            L.codec_ctx = nullptr;
        }
        if (L.udp)
        {
            L.udp->shutdown();
            L.udp.reset();
        }
    }
    (void)HAL_VIDEO_OPS.deinit(video_ctx);
    return 1;

fail_partial_codecs:
    goto fail_cleanup;
}
