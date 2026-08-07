/**
 * @file udp_stream_test.cpp
 * @brief Example: HAL media pipeline -> RTP over UDP using `HalUdpStream` from `hal-common` (bounded queue,
 * copy then release in codec callback).
 *
 * Usage:
 *   hal-udp-stream-test <medialib_json_path> <dest_host> <dest_port> [encoder_stream_id]
 *
 * If encoder_stream_id is omitted, the first H.264 or H.265 codec context from get_codec_list is used.
 * H.265 uses `HalUdpStreamMode::RtpH265AnnexB` (PT 97); H.264 uses `RtpH264AnnexB` (PT 96).
 *
 * Playback on the receiver: FFmpeg needs an SDP matching the payload type.
 * Use the `.sdp` files in this directory (`receive_h264_rtp.sdp` / `receive_h265_rtp.sdp`), set "m=video <port>"
 * to dest_port, then from this directory:
 *   ffplay -protocol_whitelist file,rtp,udp -fflags nobuffer -flags low_delay -i receive_h264_rtp.sdp
 * (Or pass the full path to the .sdp file.)
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"
#include "media/hal_codec_internal.h"
#include "media/hal_media.h"

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <csignal>
#include <signal.h>

namespace
{

std::atomic<bool> g_stop_main{false};
static volatile sig_atomic_t g_signal_pending = 0;

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

void codec_cb(void *codec_ctx, HalPacketBuffer *pkt, void *userdata)
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
    std::vector<uint8_t> copy(pkt->data, pkt->data + pkt->size);
    stream->push_annex_b(std::move(copy), pkt->timestamp_ns);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

} // namespace

int main(int argc, char **argv)
{
    auto endswith = [](const char *s, const char *suf) {
        size_t ls = std::strlen(s), lf = std::strlen(suf);
        return ls >= lf && std::strcmp(s + ls - lf, suf) == 0;
    };

    /* medialib_json_path is optional: if the first arg looks like a path it is consumed as
     * the config; otherwise the HAL uses its compiled-in default config. */
    const char *json_path = nullptr;
    int idx = 1;
    if (argc >= 2 && (std::strchr(argv[1], '/') != nullptr || endswith(argv[1], ".json") ||
                      std::strcmp(argv[1], "--default") == 0))
    {
        if (std::strcmp(argv[1], "--default") != 0)
        {
            json_path = argv[1];
        }
        idx = 2;
    }

    if (argc < idx + 2)
    {
        std::fprintf(stderr,
                     "Usage: %s [medialib_json_path|--default] <dest_host> <dest_port> [encoder_stream_id]\n"
                     "  medialib_json_path optional; omit (or pass --default) to use the HAL embedded default.\n",
                     argv[0]);
        return 1;
    }

    const char *host = argv[idx];
    const uint16_t port = static_cast<uint16_t>(std::atoi(argv[idx + 1]));
    const char *encoder_id_arg = (argc >= idx + 3) ? argv[idx + 2] : nullptr;

    HAL_LOG_INFO("hal-udp-stream-test starting (config=%s dest=%s:%u encoder_id=%s)",
                 json_path ? json_path : "(embedded default)", host, static_cast<unsigned>(port),
                 encoder_id_arg ? encoder_id_arg : "(auto)");

    install_signal_handlers();

    HalMediaConfig mcfg{};
    mcfg.config_path = json_path;
    mcfg.config_json = nullptr;
    mcfg.image_config = {};

    void *media_ctx = nullptr;
    int rc = HAL_MEDIA_OPS.init(&mcfg, &media_ctx);
    if (rc != HAL_OK || !media_ctx)
    {
        log_err("HAL_MEDIA_OPS.init failed: %d", rc);
        return 1;
    }
    HAL_LOG_INFO("media init OK");

    rc = HAL_MEDIA_OPS.start(media_ctx);
    if (rc != HAL_OK)
    {
        log_err("HAL_MEDIA_OPS.start failed: %d", rc);
        (void)HAL_MEDIA_OPS.deinit(media_ctx);
        return 1;
    }
    HAL_LOG_INFO("media pipeline started");

    void *codec_list_raw = nullptr;
    uint32_t codec_count = 0;
    rc = HAL_MEDIA_OPS.get_codec_list(media_ctx, &codec_list_raw, &codec_count);
    auto **codec_list = reinterpret_cast<void **>(codec_list_raw);
    if (rc != HAL_OK || !codec_list || codec_count == 0)
    {
        log_err("get_codec_list failed: %d", rc);
        (void)HAL_MEDIA_OPS.stop(media_ctx);
        (void)HAL_MEDIA_OPS.deinit(media_ctx);
        return 1;
    }
    HAL_LOG_INFO("codec contexts: %u", static_cast<unsigned>(codec_count));
    for (uint32_t i = 0; i < codec_count; i++)
    {
        auto *cc = static_cast<HalCodecContext *>(codec_list[i]);
        if (!cc)
        {
            HAL_LOG_INFO("  codec[%u] (null)", static_cast<unsigned>(i));
            continue;
        }
        HAL_LOG_INFO("  codec[%u] stream_id=%s packet_type=%s %ux%u @%u fps", static_cast<unsigned>(i),
                     cc->codec_name, packet_type_str(cc->config.packet_type), cc->config.width, cc->config.height,
                     cc->config.framerate);
    }

    void *codec_ctx = nullptr;
    if (encoder_id_arg)
    {
        for (uint32_t i = 0; i < codec_count; i++)
        {
            auto *cc = static_cast<HalCodecContext *>(codec_list[i]);
            if (cc && std::strcmp(cc->codec_name, encoder_id_arg) == 0)
            {
                codec_ctx = cc;
                break;
            }
        }
        if (!codec_ctx)
        {
            log_err("encoder stream id not found: %s", encoder_id_arg);
            (void)HAL_MEDIA_OPS.stop(media_ctx);
            (void)HAL_MEDIA_OPS.deinit(media_ctx);
            return 1;
        }
    }
    else
    {
        for (uint32_t i = 0; i < codec_count; i++)
        {
            auto *cc = static_cast<HalCodecContext *>(codec_list[i]);
            if (!cc)
            {
                continue;
            }
            if (cc->config.packet_type == HAL_PACKET_TYPE_H264 || cc->config.packet_type == HAL_PACKET_TYPE_H265)
            {
                codec_ctx = cc;
                break;
            }
        }
        if (!codec_ctx)
        {
            codec_ctx = codec_list[0];
        }
    }

    auto *selected_codec = static_cast<HalCodecContext *>(codec_ctx);
    HAL_LOG_INFO("using encoder stream_id=%s packet_type=%s %ux%u @%u fps", selected_codec->codec_name,
                 packet_type_str(selected_codec->config.packet_type), selected_codec->config.width,
                 selected_codec->config.height, selected_codec->config.framerate);

    HalUdpStreamConfig uscfg{};
    uscfg.host = host;
    uscfg.port = port;
    if (selected_codec->config.packet_type == HAL_PACKET_TYPE_H265)
    {
        uscfg.mode = HalUdpStreamMode::RtpH265AnnexB;
        uscfg.rtp_payload_type = 97U;
    }
    else
    {
        uscfg.mode = HalUdpStreamMode::RtpH264AnnexB;
        uscfg.rtp_payload_type = 96U;
    }
    HalUdpStream udp_stream(uscfg);
    if (!udp_stream.ok())
    {
        log_err("HalUdpStream init failed");
        (void)HAL_MEDIA_OPS.stop(media_ctx);
        (void)HAL_MEDIA_OPS.deinit(media_ctx);
        return 1;
    }

    rc = HAL_CODEC_OPS.subscribe(codec_ctx, codec_cb, &udp_stream);
    if (rc != HAL_OK)
    {
        log_err("HAL_CODEC_OPS.subscribe failed: %d", rc);
        (void)HAL_MEDIA_OPS.stop(media_ctx);
        (void)HAL_MEDIA_OPS.deinit(media_ctx);
        return 1;
    }
    HAL_LOG_INFO("subscribed to encoder packets");

    HAL_LOG_INFO("RTP %s PT=%u -> %s:%u (UDP). Ctrl+C or SIGTERM to stop.",
                 (uscfg.mode == HalUdpStreamMode::RtpH265AnnexB) ? "H.265" : "H.264",
                 static_cast<unsigned>(uscfg.rtp_payload_type), host, static_cast<unsigned>(port));

    auto t0 = std::chrono::steady_clock::now();
    auto last_print = t0;
    uint64_t last_bytes = 0;
    for (;;)
    {
        if (g_signal_pending != 0)
        {
            g_signal_pending = 0;
            HAL_LOG_INFO("signal received, shutting down");
            g_stop_main.store(true, std::memory_order_release);
            udp_stream.stop_accepting();
            break;
        }
        if (g_stop_main.load(std::memory_order_acquire))
        {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        const auto t1 = std::chrono::steady_clock::now();
        if (t1 - last_print < std::chrono::seconds(1))
        {
            continue;
        }
        last_print = t1;
        const double dt = std::chrono::duration<double>(t1 - t0).count();
        t0 = t1;
        const uint64_t b = udp_stream.bytes_sent();
        const uint64_t p = udp_stream.packets_sent();
        const double kbps = (dt > 0.0) ? ((b - last_bytes) * 8.0 / dt / 1000.0) : 0.0;
        last_bytes = b;
        HAL_LOG_INFO("rtp sent: bytes=%llu packets=%llu ~%.1f kbps", static_cast<unsigned long long>(b),
                     static_cast<unsigned long long>(p), kbps);
    }

    HAL_LOG_INFO("codec unsubscribe");
    (void)HAL_CODEC_OPS.unsubscribe(codec_ctx, codec_cb);
    udp_stream.shutdown();

    HAL_LOG_INFO("media pipeline stop");
    (void)HAL_MEDIA_OPS.stop(media_ctx);
    HAL_LOG_INFO("media deinit");
    (void)HAL_MEDIA_OPS.deinit(media_ctx);
    HAL_LOG_INFO("exit ok");
    return 0;
}
