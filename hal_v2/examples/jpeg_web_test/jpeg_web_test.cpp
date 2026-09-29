/**
 * @file jpeg_web_test.cpp
 * @brief HAL JPEG/MJPEG encode verification with a built-in web viewer.
 *
 * Verifies the HAL JPEG encode path end-to-end:
 *   1. Single-frame capture — the first encoded frame is written to
 *      /tmp/hal_jpeg_snap.jpg; /snap/new re-captures on demand.
 *   2. Continuous MJPEG stream — served over HTTP as
 *      multipart/x-mixed-replace, viewable directly in a browser.
 *
 * Platform fact (Hailo-15): JPEG/MJPEG encoding runs on the CPU — the
 * official Media Library user guide states "JPEG Encoding is not hardware
 * accelerated"; only H.264/H.265 use the VCENC hardware encoder. This
 * example therefore validates the HAL MJPEG path (quality, framerate,
 * packet delivery), NOT hardware JPEG acceleration. The encoder itself is
 * multi-threaded CPU JPEG (medialib jpeg_encoder, n_threads/quality).
 *
 * Usage:
 *   hal-jpeg-web-test [port=8080] [width=1280] [height=720] [fps=10] [quality=85]
 *
 * Web endpoints (open http://<board-ip>:<port>/ for the viewer page):
 *   /            viewer page (stream + snapshot links + stats)
 *   /stream      MJPEG stream (multipart/x-mixed-replace)
 *   /snap.jpg    latest captured JPEG frame
 *   /snap/new    capture the NEXT frame -> /tmp/hal_jpeg_snap.jpg + return it
 *   /stats       JSON stats (frames, fps, last frame size)
 *
 * Stop with Ctrl+C (the pipeline is torn down by process exit).
 */

#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "media/hal_codec.h"
#include "media/hal_isp.h"
#include "media/hal_video.h"
#include "media/hal_video_internal.h"
#include "media/hal_codec_internal.h"
#include "media/hal_media.h"

#include "httplib.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr const char *kJpegStreamId = "mjpeg0";
constexpr const char *kSnapPath = "/tmp/hal_jpeg_snap.jpg";

struct JpegStore
{
    std::mutex mu;
    std::condition_variable cv;
    std::vector<uint8_t> latest;
    uint64_t frames = 0;
    uint64_t bytes_total = 0;
    std::chrono::steady_clock::time_point t0;
    std::atomic<bool> want_capture{false}; /* /snap/new requested */
    bool first_saved = false;

    void push(const uint8_t *data, uint32_t size)
    {
        bool do_save = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            latest.assign(data, data + size);
            frames++;
            bytes_total += size;
            if (!first_saved || want_capture.load())
            {
                do_save = true;
                want_capture.store(false);
                first_saved = true;
            }
        }
        if (do_save)
        {
            /* Best-effort single-frame capture to file (outside the lock). */
            if (FILE *f = std::fopen(kSnapPath, "wb"))
            {
                std::fwrite(data, 1, size, f);
                std::fclose(f);
                std::printf("[jpeg] saved single frame: %s (%u bytes)\n", kSnapPath, size);
                std::fflush(stdout);
            }
        }
        cv.notify_all();
    }

    bool wait_next(std::vector<uint8_t> &out, uint64_t after_seq, int timeout_ms)
    {
        std::unique_lock<std::mutex> lk(mu);
        return cv.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] {
            return frames > after_seq && !latest.empty();
        }) && ([&] {
            out = latest;
            return true;
        })();
    }
};

JpegStore g_store;
void *g_media_ctx = nullptr;

void packet_cb(void *codec_ctx, HalPacketBuffer *pkt, void *user)
{
    (void)codec_ctx;
    (void)user;
    if (!pkt || pkt->type != HAL_PACKET_TYPE_MJPEG || !pkt->data || pkt->size == 0)
    {
        return;
    }
    /* The packet is mmap'd DMA memory: copy now, release immediately. */
    g_store.push(pkt->data, pkt->size);
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

std::string stats_json()
{
    std::lock_guard<std::mutex> lk(g_store.mu);
    const double secs =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - g_store.t0).count();
    const double fps = secs > 0.0 ? static_cast<double>(g_store.frames) / secs : 0.0;
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "{\"frames\":%llu,\"fps\":%.2f,\"last_frame_bytes\":%zu,\"total_bytes\":%llu}",
                  static_cast<unsigned long long>(g_store.frames), fps, g_store.latest.size(),
                  static_cast<unsigned long long>(g_store.bytes_total));
    return buf;
}

const char *kIndexHtml =
    "<!DOCTYPE html><html><head><title>HAL JPEG test</title>"
    "<style>body{font-family:monospace;background:#111;color:#eee;margin:24px}"
    "img{max-width:100%;border:1px solid #444}</style></head><body>"
    "<h2>HAL MJPEG encode test</h2>"
    "<img src=\"/stream\" alt=\"MJPEG stream\">"
    "<p><a href=\"/snap.jpg\" download=\"frame.jpg\">[save latest frame]</a> "
    "<a href=\"/snap/new\" target=\"_blank\">[capture next frame -> /tmp/hal_jpeg_snap.jpg]</a> "
    "<a href=\"/stats\">[stats]</a></p>"
    "<p id=\"s\"></p><script>fetch('/stats').then(r=>r.json()).then(j=>"
    "document.getElementById('s').textContent="
    "'frames='+j.frames+' fps='+j.fps+' last='+j.last_frame_bytes+'B').catch(()=>{});"
    "setInterval(()=>fetch('/stats').then(r=>r.json()).then(j=>"
    "document.getElementById('s').textContent="
    "'frames='+j.frames+' fps='+j.fps+' last='+j.last_frame_bytes+'B').catch(()=>{}),2000)</script>"
    "</body></html>";

} // namespace

int main(int argc, char **argv)
{
    const int port = (argc >= 2) ? std::atoi(argv[1]) : 8080;
    const uint32_t width = (argc >= 3) ? static_cast<uint32_t>(std::atoi(argv[2])) : 1280U;
    const uint32_t height = (argc >= 4) ? static_cast<uint32_t>(std::atoi(argv[3])) : 720U;
    const uint32_t fps = (argc >= 5) ? static_cast<uint32_t>(std::atoi(argv[4])) : 10U;
    const uint32_t quality = (argc >= 6) ? static_cast<uint32_t>(std::atoi(argv[5])) : 85U;

    if (!HAL_MEDIA_OPS.init || !HAL_CODEC_OPS.subscribe)
    {
        std::fprintf(stderr, "HAL media/codec ops unavailable\n");
        return 3;
    }

    /* 1. Bring up the media pipeline with the embedded default profile. */
    HalMediaConfig mcfg{};
    mcfg.config_path = nullptr; /* embedded default (Daylight) */
    mcfg.config_json = nullptr;
    mcfg.image_config = {};
    std::printf("init media (embedded default profile)...\n");
    int rc = HAL_MEDIA_OPS.init(&mcfg, &g_media_ctx);
    if (rc != HAL_OK || !g_media_ctx)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.init failed: %d (%s)\n"
                             "(stub platform: this example requires real media hardware)\n",
                     rc, hal_error_to_string(static_cast<HalErrorCode>(rc)));
        return 1;
    }
    rc = HAL_MEDIA_OPS.start(g_media_ctx);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_MEDIA_OPS.start failed: %d\n", rc);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    /* 2. Add an MJPEG stream (frontend output + encoder, same id) via add_streams_batch.
     * A codec-only add has no frontend source, so auto-feed would never deliver frames;
     * the batch form creates the multi-resize output the encoder taps. */
    HalMediaAddCodecConfig add{};
    add.stream_id = kJpegStreamId;
    add.codec.type = HAL_CODEC_TYPE_FROM_MEDIA;
    add.codec.packet_type = HAL_PACKET_TYPE_MJPEG;
    add.codec.width = width;
    add.codec.height = height;
    add.codec.framerate = fps;
    add.codec.format = HAL_PIX_FMT_NV12;
    add.codec.jpeg_quality = quality;

    HalMediaAddVideoConfig vadd{};
    vadd.stream_id = kJpegStreamId;
    vadd.video.type = HAL_VIDEO_TYPE_FROM_MEDIA;
    vadd.video.width = width;
    vadd.video.height = height;
    vadd.video.framerate = fps;
    vadd.video.format = HAL_PIX_FMT_NV12;

    rc = HAL_MEDIA_OPS.add_streams_batch ? HAL_MEDIA_OPS.add_streams_batch(g_media_ctx, &add, &vadd)
                                         : HAL_ERR_NOT_SUPPORTED;
    std::printf("add_streams_batch(mjpeg %ux%u@%u q%u) ret=%d (%s)\n", width, height, fps, quality, rc,
                hal_error_to_string(static_cast<HalErrorCode>(rc)));
    if (rc != HAL_OK)
    {
        (void)HAL_MEDIA_OPS.stop(g_media_ctx);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }

    /* 3. Find the new codec context (profile-mapped sink id) and subscribe. */
    void *cl = nullptr;
    uint32_t cc = 0;
    rc = HAL_MEDIA_OPS.get_codec_list(g_media_ctx, &cl, &cc);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "get_codec_list failed: %d\n", rc);
        return 1;
    }
    HalCodecContext *codec_ctx = nullptr;
    auto **list = reinterpret_cast<void **>(cl);
    for (uint32_t i = 0; i < cc; ++i)
    {
        auto *c = static_cast<HalCodecContext *>(list[i]);
        if (c && c->config.packet_type == HAL_PACKET_TYPE_MJPEG)
        {
            codec_ctx = c; /* the only MJPEG stream in this pipeline */
            break;
        }
    }
    if (!codec_ctx)
    {
        std::fprintf(stderr, "no MJPEG codec stream in list (%u streams):\n", cc);
        for (uint32_t i = 0; i < cc; ++i)
        {
            auto *c = static_cast<HalCodecContext *>(list[i]);
            std::fprintf(stderr, "  [%u] name='%s' packet_type=%d %ux%u@%u\n", i,
                         c ? c->codec_name : "(null)", c ? (int)c->config.packet_type : -1,
                         c ? c->config.width : 0U, c ? c->config.height : 0U, c ? c->config.framerate : 0U);
        }
        (void)HAL_MEDIA_OPS.stop(g_media_ctx);
        (void)HAL_MEDIA_OPS.deinit(g_media_ctx);
        return 1;
    }
    std::printf("mjpeg encoder stream: '%s'\n", codec_ctx->codec_name);

    /* New streams default to manual feed; enable frontend->encoder forwarding. */
    if (HAL_MEDIA_OPS.set_encoder_auto_feed_for_stream)
    {
        rc = HAL_MEDIA_OPS.set_encoder_auto_feed_for_stream(g_media_ctx, codec_ctx->codec_name, true);
        std::printf("set_encoder_auto_feed_for_stream(%s, true) ret=%d\n", codec_ctx->codec_name, rc);
    }
    g_store.t0 = std::chrono::steady_clock::now();
    rc = HAL_CODEC_OPS.subscribe(codec_ctx, packet_cb, nullptr);
    if (rc != HAL_OK)
    {
        std::fprintf(stderr, "HAL_CODEC_OPS.subscribe failed: %d\n", rc);
        return 1;
    }

    /* Optional manual-WB verification hook: HAL_JPEG_WB="r,gr,gb,b" (e.g. "3.9,1,1,1")
     * applies manual white balance after the stream is up; combine two runs to
     * compare the visual effect of manual gains. */
    if (const char *wb = std::getenv("HAL_JPEG_WB"))
    {
        HalIspWbConfig cfg{};
        cfg.manual_state = true;
        cfg.r_gain = cfg.gr_gain = cfg.gb_gain = cfg.b_gain = 1.0f;
        if (std::sscanf(wb, "%f,%f,%f,%f", &cfg.r_gain, &cfg.gr_gain, &cfg.gb_gain, &cfg.b_gain) == 4 &&
            HAL_ISP_OPS.set_wb_config)
        {
            void *vl = nullptr;
            uint32_t vcn = 0;
            if (HAL_MEDIA_OPS.get_video_list(g_media_ctx, &vl, &vcn) == HAL_OK && vcn > 0)
            {
                auto *vctx = static_cast<HalVideoContext *>(reinterpret_cast<void **>(vl)[0]);
                const int r = HAL_ISP_OPS.set_wb_config(vctx, &cfg);
                std::printf("[wb] manual set ret=%d (r=%.2f gr=%.2f gb=%.2f b=%.2f)\n", r, cfg.r_gain,
                            cfg.gr_gain, cfg.gb_gain, cfg.b_gain);
            }
        }
    }

    /* 4. Wait for the first frame (single-frame encode verification). */
    std::vector<uint8_t> first;
    if (!g_store.wait_next(first, 0, 5000))
    {
        std::fprintf(stderr, "no MJPEG frame within 5s — check sensor / pipeline\n");
        return 1;
    }
    std::printf("[jpeg] first frame: %zu bytes (also saved to %s)\n", first.size(), kSnapPath);

    /* 5. Serve the web viewer. */
    httplib::Server svr;

    svr.Get("/", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(kIndexHtml, "text/html");
    });

    svr.Get("/stats", [](const httplib::Request &, httplib::Response &res) {
        res.set_content(stats_json(), "application/json");
    });

    svr.Get("/snap.jpg", [](const httplib::Request &, httplib::Response &res) {
        std::lock_guard<std::mutex> lk(g_store.mu);
        if (g_store.latest.empty())
        {
            res.status = 503;
            res.set_content("no frame yet", "text/plain");
            return;
        }
        res.set_content(reinterpret_cast<const char *>(g_store.latest.data()),
                        g_store.latest.size(), "image/jpeg");
    });

    svr.Get("/snap/new", [](const httplib::Request &, httplib::Response &res) {
        uint64_t before = 0;
        {
            std::lock_guard<std::mutex> lk(g_store.mu);
            before = g_store.frames;
            g_store.want_capture.store(true);
        }
        std::vector<uint8_t> frame;
        if (!g_store.wait_next(frame, before, 5000))
        {
            res.status = 503;
            res.set_content("no new frame within 5s", "text/plain");
            return;
        }
        res.set_content(reinterpret_cast<const char *>(frame.data()), frame.size(), "image/jpeg");
    });

    svr.Get("/stream", [](const httplib::Request &, httplib::Response &res) {
        res.status = 200;
        res.set_header("Cache-Control", "no-store");
        res.set_chunked_content_provider(
            "multipart/x-mixed-replace; boundary=halframe",
            [](size_t /*offset*/, httplib::DataSink &sink) -> bool {
                uint64_t last = 0;
                {
                    std::lock_guard<std::mutex> lk(g_store.mu);
                    last = g_store.frames;
                }
                std::vector<uint8_t> frame;
                if (!g_store.wait_next(frame, last, 10000))
                {
                    return false; /* no frame for 10s: end the stream */
                }
                static const char head[] = "\r\n--halframe\r\nContent-Type: image/jpeg\r\n\r\n";
                if (!sink.write(head, sizeof(head) - 1))
                {
                    return false;
                }
                if (!sink.write(reinterpret_cast<const char *>(frame.data()), frame.size()))
                {
                    return false;
                }
                static const char tail[] = "\r\n";
                return sink.write(tail, sizeof(tail) - 1);
            });
    });

    std::printf("\n=== HAL JPEG web test ===\n");
    std::printf("MJPEG stream : http://0.0.0.0:%d/stream\n", port);
    std::printf("viewer page  : http://<board-ip>:%d/\n", port);
    std::printf("snapshot     : http://<board-ip>:%d/snap.jpg (file: %s)\n", port, kSnapPath);
    std::printf("capture new  : http://<board-ip>:%d/snap/new\n", port);
    std::printf("note         : JPEG encode is CPU (SW) on Hailo-15, not VCENC HW\n");
    std::fflush(stdout);

    if (!svr.listen("0.0.0.0", static_cast<int>(port)))
    {
        std::fprintf(stderr, "http server failed to listen on port %d\n", port);
        return 1;
    }
    return 0;
}
