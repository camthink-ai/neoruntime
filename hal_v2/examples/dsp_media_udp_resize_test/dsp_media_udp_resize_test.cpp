/**
 * @file dsp_media_udp_resize_test.cpp
 * @brief Example: medialib frontend -> DSP resize -> encoder -> RTP/UDP.
 *
 * - Uses HAL_MEDIA_OPS to create a pipeline from a medialib JSON.
 * - Disables media auto-feed so the app can insert a DSP resize step.
 * - Subscribes to a preview frontend, resizes frames via HAL_DSP_OPS, then
 *   feeds the resized frames into HAL_CODEC_OPS.
 * - Encoded H.264/H.265 packets are pushed to a UDP socket using HalUdpStream.
 *
 * Usage:
 *   hal-dsp-media-udp-resize-test <medialib_json> <host> <port> [out_w out_h]
 *
 * The example uses the first frontend + first encoder from the profile.
 */

#include "common/hal_log.h"
#include "common/hal_udp_stream.hpp"

#include "media/hal_media.h"
#include "media/hal_media_helpers.h"
#include "media/hal_video.h"
#include "media/hal_codec.h"

#include "dsp/hal_dsp.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <condition_variable>
#include <vector>
#include <csignal>

namespace
{

std::atomic<bool> g_stop{false};
std::atomic<uint64_t> g_frames_in{0};
std::atomic<uint64_t> g_frames_resized{0};
std::atomic<uint64_t> g_frames_fed{0};
std::atomic<uint64_t> g_pkts_out{0};
std::atomic<uint64_t> g_bytes_out{0};

struct FrameJob
{
    void *video_ctx = nullptr;
    HalFrameBuffer *frame = nullptr; /* heap-allocated copy of callback frame */
};

struct AppCtx
{
    void *media_ctx = nullptr;
    void *video_ctx = nullptr;
    void *codec_ctx = nullptr;
    void *dsp_ctx = nullptr;

    HalUdpStream *udp = nullptr;
    std::string stream_key;

    uint32_t out_w = 0;
    uint32_t out_h = 0;

    std::mutex frame_q_mu;
    std::condition_variable frame_q_cv;
    std::deque<FrameJob> frame_q;
    static constexpr size_t kFrameQueueMax = 4;
    std::thread worker;

    /* Keep a small window of in-flight encoder input buffers alive. */
    std::mutex inflight_mu;
    std::deque<HalFrameBuffer *> inflight;
    static constexpr size_t kInflightMax = 8;
};

static HalFrameBuffer *heap_clone_frame_from_callback(HalFrameBuffer *stack_frame)
{
    if (!stack_frame)
        return nullptr;
    auto *heap = new (std::nothrow) HalFrameBuffer{};
    if (!heap)
        return nullptr;
    *heap = *stack_frame;
    stack_frame->priv = nullptr;
    return heap;
}

static void release_async_job_frame(void *video_ctx, HalFrameBuffer *heap_frame)
{
    if (!heap_frame)
        return;
    (void)HAL_VIDEO_OPS.release_frame(video_ctx, heap_frame);
    delete heap_frame;
}

static void enqueue_frame_job(AppCtx *ctx, void *video_ctx, HalFrameBuffer *heap_frame)
{
    if (!ctx || !video_ctx || !heap_frame)
        return;
    std::unique_lock<std::mutex> lk(ctx->frame_q_mu);
    while (ctx->frame_q.size() >= AppCtx::kFrameQueueMax && !g_stop.load(std::memory_order_acquire))
    {
        FrameJob drop = ctx->frame_q.front();
        ctx->frame_q.pop_front();
        lk.unlock();
        release_async_job_frame(drop.video_ctx, drop.frame);
        lk.lock();
    }
    if (g_stop.load(std::memory_order_acquire))
    {
        lk.unlock();
        release_async_job_frame(video_ctx, heap_frame);
        return;
    }
    ctx->frame_q.push_back(FrameJob{video_ctx, heap_frame});
    ctx->frame_q_cv.notify_one();
}

static void video_callback(void *video_ctx, HalFrameBuffer *frame, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !frame)
        return;
    g_frames_in.fetch_add(1, std::memory_order_relaxed);
    HalFrameBuffer *heap = heap_clone_frame_from_callback(frame);
    if (!heap)
    {
        HAL_LOG_ERROR("dsp_media_udp_resize: failed to clone frame");
        return;
    }
    if (!video_ctx)
    {
        delete heap;
        return;
    }
    enqueue_frame_job(ctx, video_ctx, heap);
}

static void codec_callback(void *codec_ctx, HalPacketBuffer *packet, void *userdata)
{
    auto *ctx = static_cast<AppCtx *>(userdata);
    if (!ctx || !packet)
        return;

    if (ctx->udp && ctx->udp->ok() && packet->data && packet->size > 0)
    {
        ctx->udp->push_annex_b(packet->data, packet->size, packet->timestamp_ns);
        g_pkts_out.fetch_add(1, std::memory_order_relaxed);
        g_bytes_out.fetch_add(packet->size, std::memory_order_relaxed);
    }

    (void)HAL_CODEC_OPS.release_packet(codec_ctx, packet);

    /* Best-effort: free one in-flight input frame per output packet.
     *
     * There is no explicit "frame consumed" callback in HAL_CODEC_OPS today.
     * In practice for H264/H265 RTP AnnexB, this callback is typically invoked
     * once per encoded frame (access unit), so releasing one buffer here keeps
     * the request pool from exhausting. */
    {
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
        {
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(to_free);
        }
    }
}

static void worker_loop(AppCtx *ctx)
{
    if (!ctx || !ctx->codec_ctx || !ctx->dsp_ctx)
        return;

    while (!g_stop.load(std::memory_order_acquire))
    {
        FrameJob job{};
        {
            std::unique_lock<std::mutex> lk(ctx->frame_q_mu);
            ctx->frame_q_cv.wait(lk, [&] {
                return g_stop.load(std::memory_order_acquire) || !ctx->frame_q.empty();
            });
            if (g_stop.load(std::memory_order_acquire))
                break;
            job = ctx->frame_q.front();
            ctx->frame_q.pop_front();
        }

        if (!job.video_ctx || !job.frame)
        {
            continue;
        }

        HalFrameBuffer *resized = nullptr;
        HalFrameBufferRequest req{};
        req.width = ctx->out_w;
        req.height = ctx->out_h;
        req.format = HAL_PIX_FMT_NV12;
        req.mem_type = HAL_MEM_DMABUF;
        req.zero_initialize = false;
        int arc = HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &resized);
        if (arc != HAL_OK || !resized)
        {
            HAL_LOG_ERROR("dsp_media_udp_resize: request_frame_buffer failed rc=%d (%ux%u)",
                          arc,
                          ctx->out_w,
                          ctx->out_h);
            release_async_job_frame(job.video_ctx, job.frame);
            continue;
        }

        /* Best-effort: clone platform/media metadata from source -> destination (see HAL_FRAME_BUFFER_OPS). */
        (void)HAL_FRAME_BUFFER_OPS.copy_metadata_from_frame_buffer(job.frame, resized);

        HalDspResizeParams rp{};
        rp.src = job.frame;
        rp.dst = resized;
        rp.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;

        int rc = HAL_DSP_OPS.resize(ctx->dsp_ctx, &rp);
        if (rc != HAL_OK)
        {
            HAL_LOG_ERROR("dsp_media_udp_resize: DSP resize failed rc=%d", rc);
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(resized);
        }
        else
        {
            g_frames_resized.fetch_add(1, std::memory_order_relaxed);
            rc = HAL_CODEC_OPS.input_frame(ctx->codec_ctx, resized);
            if (rc != HAL_OK)
            {
                HAL_LOG_ERROR("dsp_media_udp_resize: codec input_frame failed rc=%d", rc);
                (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(resized);
            }
            else
            {
                g_frames_fed.fetch_add(1, std::memory_order_relaxed);
                {
                    std::lock_guard<std::mutex> lk(ctx->inflight_mu);
                    ctx->inflight.push_back(resized);
                }
                while (true)
                {
                    HalFrameBuffer *oldest = nullptr;
                    {
                        std::lock_guard<std::mutex> lk(ctx->inflight_mu);
                        if (ctx->inflight.size() <= AppCtx::kInflightMax)
                            break;
                        oldest = ctx->inflight.front();
                        ctx->inflight.pop_front();
                    }
                    if (oldest)
                        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(oldest);
                }
            }
        }

        release_async_job_frame(job.video_ctx, job.frame);
    }

    {
        std::lock_guard<std::mutex> lk(ctx->frame_q_mu);
        for (const auto &job : ctx->frame_q)
        {
            if (job.video_ctx && job.frame)
                release_async_job_frame(job.video_ctx, job.frame);
        }
        ctx->frame_q.clear();
    }

    while (!ctx->inflight.empty())
    {
        HalFrameBuffer *oldest = nullptr;
        {
            std::lock_guard<std::mutex> lk(ctx->inflight_mu);
            if (ctx->inflight.empty())
                break;
            oldest = ctx->inflight.front();
            ctx->inflight.pop_front();
        }
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(oldest);
    }
}

} // namespace

static void on_sigint(int)
{
    g_stop.store(true, std::memory_order_release);
}

int main(int argc, char **argv)
{
    auto endswith = [](const char *s, const char *suf) {
        size_t ls = std::strlen(s), lf = std::strlen(suf);
        return ls >= lf && std::strcmp(s + ls - lf, suf) == 0;
    };

    /* medialib_json is optional: if the first arg looks like a path it is consumed as the
     * config; otherwise the HAL uses its compiled-in default config. */
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
                     "Usage: %s [medialib_json|--default] <host> <port> [out_w out_h] [--profile NAME]\n"
                     "  medialib_json optional; omit (or pass --default) to use the HAL embedded default.\n",
                     argv[0]);
        return EXIT_FAILURE;
    }

    const char *host = argv[idx];
    uint16_t port = static_cast<uint16_t>(std::strtoul(argv[idx + 1], nullptr, 10));

    uint32_t cli_out_w = 0;
    uint32_t cli_out_h = 0;
    if (argc >= idx + 4)
    {
        cli_out_w = static_cast<uint32_t>(std::strtoul(argv[idx + 2], nullptr, 10));
        cli_out_h = static_cast<uint32_t>(std::strtoul(argv[idx + 3], nullptr, 10));
    }

    const char *cli_profile = nullptr;
    for (int i = idx; i + 1 < argc; i++)
    {
        if (std::strcmp(argv[i], "--profile") == 0)
        {
            cli_profile = argv[i + 1];
            break;
        }
    }

    AppCtx ctx{};

    HalMediaConfig mcfg{};
    mcfg.config_path = json_path;

    int rc = HAL_MEDIA_OPS.init(&mcfg, &ctx.media_ctx);
    if (rc != HAL_OK || !ctx.media_ctx)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.init failed rc=%d\n", rc);
        return EXIT_FAILURE;
    }

    if (cli_profile && cli_profile[0] != '\0')
    {
        HAL_LOG_INFO("dsp_media_udp_resize: switching profile -> \"%s\"", cli_profile);
        int prc = HAL_MEDIA_OPS.switch_profile(ctx.media_ctx, cli_profile, false);
        if (prc != HAL_OK)
        {
            std::fprintf(stderr, "switch_profile(\"%s\") failed rc=%d\n", cli_profile, prc);
            HAL_MEDIA_OPS.deinit(ctx.media_ctx);
            return EXIT_FAILURE;
        }
    }
    /* We feed the encoder manually after DSP resize. */
    (void)HAL_MEDIA_OPS.set_encoder_auto_feed(ctx.media_ctx, false);

    void *video_list_raw = nullptr;
    uint32_t video_count = 0;
    rc = HAL_MEDIA_OPS.get_video_list(ctx.media_ctx, &video_list_raw, &video_count);
    auto **video_list = reinterpret_cast<void **>(video_list_raw);
    if (rc != HAL_OK || !video_list || video_count == 0)
    {
        std::fprintf(stderr, "get_video_list failed rc=%d count=%u\n", rc, video_count);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    ctx.video_ctx = video_list[0];

    void *codec_list_raw = nullptr;
    uint32_t codec_count = 0;
    rc = HAL_MEDIA_OPS.get_codec_list(ctx.media_ctx, &codec_list_raw, &codec_count);
    auto **codec_list = reinterpret_cast<void **>(codec_list_raw);
    if (rc != HAL_OK || !codec_list || codec_count == 0)
    {
        std::fprintf(stderr, "get_codec_list failed rc=%d count=%u\n", rc, codec_count);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }
    ctx.codec_ctx = codec_list[0];

    HalCodecConfig ccfg{};
    rc = HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "get_current_config(codec) failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    if (cli_out_w > 0 && cli_out_h > 0)
    {
        ctx.out_w = cli_out_w;
        ctx.out_h = cli_out_h;

        if (ctx.out_w != ccfg.width || ctx.out_h != ccfg.height)
        {
            HalCodecConfig new_cfg{};
            new_cfg.width = ctx.out_w;
            new_cfg.height = ctx.out_h;
            int drc = HAL_CODEC_OPS.dynamic_change_config(ctx.codec_ctx, &new_cfg);
            if (drc != HAL_OK)
            {
                std::fprintf(stderr,
                             "dynamic_change_config(codec) failed rc=%d (requested %ux%u)\n",
                             drc,
                             ctx.out_w,
                             ctx.out_h);
                HAL_MEDIA_OPS.deinit(ctx.media_ctx);
                return EXIT_FAILURE;
            }

            rc = HAL_CODEC_OPS.get_current_config(ctx.codec_ctx, &ccfg);
            if (rc == HAL_OK)
            {
                ctx.out_w = ccfg.width;
                ctx.out_h = ccfg.height;
            }
        }
    }
    else
    {
        ctx.out_w = ccfg.width;
        ctx.out_h = ccfg.height;
    }

    HalUdpStreamConfig ucfg{};
    ucfg.host = host;
    ucfg.port = port;
    ucfg.mode = (ccfg.packet_type == HAL_PACKET_TYPE_H265) ? HalUdpStreamMode::RtpH265AnnexB
                                                           : HalUdpStreamMode::RtpH264AnnexB;
    HalUdpStream udp(ucfg);
    ctx.udp = &udp;
    if (!udp.ok())
    {
        std::fprintf(stderr, "HalUdpStream init failed (host=%s port=%u)\n", host, port);
    }

    HalDspConfig dcfg{};
    dcfg.device_priority = 0;
    rc = HAL_DSP_OPS.init(&dcfg, &ctx.dsp_ctx);
    if (rc != HAL_OK || !ctx.dsp_ctx)
    {
        std::fprintf(stderr, "HAL_DSP_OPS.init failed rc=%d\n", rc);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    rc = HAL_CODEC_OPS.subscribe(ctx.codec_ctx, codec_callback, &ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "codec subscribe failed rc=%d\n", rc);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    auto *vctx = static_cast<HalVideoContext *>(ctx.video_ctx);
    ctx.stream_key = (vctx && vctx->video_name[0]) ? std::string(vctx->video_name) : std::string();
    HAL_LOG_INFO("dsp_media_udp_resize: subscribe frontend stream_key=\"%s\" out=%ux%u",
                 ctx.stream_key.c_str(),
                 ctx.out_w,
                 ctx.out_h);

    rc = HAL_VIDEO_OPS.subscribe_stream(ctx.video_ctx, ctx.stream_key.c_str(), video_callback, &ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "video subscribe_stream failed rc=%d\n", rc);
        HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_callback);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    rc = HAL_MEDIA_OPS.start(ctx.media_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.start failed rc=%d\n", rc);
        HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
        HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_callback);
        HAL_DSP_OPS.deinit(ctx.dsp_ctx);
        HAL_MEDIA_OPS.deinit(ctx.media_ctx);
        return EXIT_FAILURE;
    }

    ctx.worker = std::thread(worker_loop, &ctx);
    HAL_LOG_INFO("dsp_media_udp_resize: worker enabled (manual DSP resize + manual encoder feed)");

    std::printf("Running DSP resize -> encode -> UDP. Press Ctrl+C to stop.\n");
    std::signal(SIGINT, on_sigint);
    while (!g_stop.load(std::memory_order_acquire))
    {
        uint64_t fi = g_frames_in.load(std::memory_order_relaxed);
        uint64_t fr = g_frames_resized.load(std::memory_order_relaxed);
        uint64_t ff = g_frames_fed.load(std::memory_order_relaxed);
        uint64_t po = g_pkts_out.load(std::memory_order_relaxed);
        uint64_t bo = g_bytes_out.load(std::memory_order_relaxed);
        HAL_LOG_INFO("stats: frames_in=%llu resized=%llu fed=%llu pkts_out=%llu bytes_out=%llu",
                     (unsigned long long)fi,
                     (unsigned long long)fr,
                     (unsigned long long)ff,
                     (unsigned long long)po,
                     (unsigned long long)bo);
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    HAL_MEDIA_OPS.stop(ctx.media_ctx);

    HAL_VIDEO_OPS.unsubscribe_stream(ctx.video_ctx, ctx.stream_key.c_str());
    HAL_CODEC_OPS.unsubscribe(ctx.codec_ctx, codec_callback);

    ctx.frame_q_cv.notify_all();
    if (ctx.worker.joinable())
        ctx.worker.join();

    udp.stop_accepting();
    udp.shutdown();

    HAL_DSP_OPS.deinit(ctx.dsp_ctx);
    HAL_MEDIA_OPS.deinit(ctx.media_ctx);

    return EXIT_SUCCESS;
}

