/**
 * @file codec_effect_test.cpp
 * @brief Effect-level verification of encoder features on real streams:
 *   1. A2 force_idr  — count IDR slices (H.264 NAL 5/7) in subscribed packets
 *                      before/after force_idr.
 *   2. A1 ROI        — compare encoder bitrate stats with smart encoding
 *                      off vs on (background_qp_delta=15, small ROI).
 *   3. B4 AE stats   — dump hist/luma grid validity after the u8 fix.
 */
#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "media/hal_codec.h"
#include "media/hal_codec_internal.h"
#include "media/hal_isp.h"
#include "media/hal_media.h"
#include "media/hal_video_internal.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

namespace
{
struct Sink
{
    void *media = nullptr;
    HalCodecContext *cc = nullptr;
    uint64_t idr_slices = 0;
    uint64_t total_nals = 0;
};

Sink g_sink;

void packet_cb(void *codec_ctx, HalPacketBuffer *pkt, void *user)
{
    (void)user;
    Sink *s = &g_sink;
    /* Annex-B scan: count IDR slices (nal type 5) and SPS (7) */
    const uint8_t *d = pkt->data;
    size_t i = 0;
    while (i + 4 < pkt->size)
    {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 0 && d[i + 3] == 1)
        {
            const uint8_t nal = d[i + 4] & 0x1f;
            if (nal == 5) s->idr_slices++;
            if (nal != 0) s->total_nals++;
            i += 4;
        }
        else if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1)
        {
            const uint8_t nal = d[i + 3] & 0x1f;
            if (nal == 5) s->idr_slices++;
            if (nal != 0) s->total_nals++;
            i += 3;
        }
        else
        {
            ++i;
        }
    }
    (void)HAL_CODEC_OPS.release_packet(codec_ctx, pkt);
}

double stats_bitrate(void *media)
{
    void *cl = nullptr;
    uint32_t n = 0;
    if (HAL_MEDIA_OPS.get_codec_list(media, &cl, &n) != HAL_OK || n == 0) return -1;
    auto **list = reinterpret_cast<void **>(cl);
    HalCodecContext *cc = static_cast<HalCodecContext *>(list[0]); /* sink0, 1080p */
    HalCodecStreamStats st{};
    if (HAL_CODEC_OPS.get_stream_stats(cc, &st) != HAL_OK) return -1;
    return (double)st.bitrate_kbps;
}
} // namespace

int main()
{
    HalMediaConfig mcfg{};
    void *media = nullptr;
    int rc = HAL_MEDIA_OPS.init(&mcfg, &media);
    if (rc != 0 || !media) { std::printf("init rc=%d\n", rc); return 1; }
    rc = HAL_MEDIA_OPS.start(media);
    std::printf("start rc=%d\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(4));

    /* ---- B4: AE stats after u8 fix ---- */
    {
        void *vl = nullptr; uint32_t vn = 0;
        if (HAL_MEDIA_OPS.get_video_list(media, &vl, &vn) == HAL_OK && vn > 0)
        {
            auto *vc = static_cast<HalVideoContext *>(reinterpret_cast<void **>(vl)[0]);
            static HalIspAeStats st;
            int r = HAL_ISP_OPS.get_ae_stats ? HAL_ISP_OPS.get_ae_stats(vc, &st) : -1;
            uint32_t lsum = 0;
            for (int i = 0; i < HAL_ISP_AE_LUMA_GRID; ++i) lsum += st.luma[i];
            std::printf("B4 ae_stats rc=%d hist_valid=%d luma_valid=%d luma_sum=%u luma[12]=%u\n",
                        r, (int)st.hist_valid, (int)st.luma_valid, lsum, st.luma[12]);
        }
    }

    /* ---- A2: IDR counting around force_idr ---- */
    void *cl = nullptr; uint32_t cn = 0;
    HAL_MEDIA_OPS.get_codec_list(media, &cl, &cn);
    g_sink.cc = static_cast<HalCodecContext *>(reinterpret_cast<void **>(cl)[0]);
    g_sink.media = media;
    rc = HAL_CODEC_OPS.subscribe(g_sink.cc, packet_cb, nullptr);
    std::printf("subscribe rc=%d\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(5));
    const uint64_t idr_before = g_sink.idr_slices;
    const double br_before = stats_bitrate(media);
    std::printf("A2 baseline: idr=%llu bitrate=%.0f kbps\n",
                (unsigned long long)idr_before, br_before);

    rc = HAL_CODEC_OPS.force_idr ? HAL_CODEC_OPS.force_idr(g_sink.cc) : -1;
    std::printf("force_idr rc=%d\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(3));
    std::printf("A2 after force: idr=%llu (delta=%lld) -> %s\n",
                (unsigned long long)g_sink.idr_slices,
                (long long)(g_sink.idr_slices - idr_before),
                (g_sink.idr_slices > idr_before) ? "PASS" : "FAIL");

    /* ---- A1: ROI bitrate effect ---- */
    HalCodecRoiConfig roi{};
    roi.enabled = true;
    roi.background_qp_delta = 15;
    roi.roi_count = 1;
    roi.rois[0] = {0.3f, 0.3f, 0.4f, 0.4f};
    rc = HAL_CODEC_OPS.set_roi_config ? HAL_CODEC_OPS.set_roi_config(g_sink.cc, &roi) : -1;
    std::printf("A1 roi_on rc=%d (bg_qp=15)\n", rc);
    std::this_thread::sleep_for(std::chrono::seconds(8)); /* monitor window ~1s, keep margin */
    const double br_roi = stats_bitrate(media);
    std::printf("A1 roi bitrate: %.0f -> %.0f kbps (%.1f%%) %s\n",
                br_before, br_roi, 100.0 * (br_roi - br_before) / (br_before > 0 ? br_before : 1.0),
                (br_before > 0 && br_roi < br_before * 0.92) ? "PASS" : "WEAK/FAIL");

    roi.enabled = false;
    (void)HAL_CODEC_OPS.set_roi_config(g_sink.cc, &roi);
    (void)HAL_CODEC_OPS.unsubscribe(g_sink.cc, packet_cb);
    (void)HAL_MEDIA_OPS.stop(media);
    (void)HAL_MEDIA_OPS.deinit(media);
    std::printf("done\n");
    return 0;
}
