/**
 * @file dsp_rotate_dewarp_test.cpp
 * @brief HAL DSP rotate (arbitrary angle) + mesh dewarp verification.
 *
 * Self-contained: synthesizes an NV12 test pattern (diagonal color quadrants),
 * runs identity-mesh dewarp (output must match input) and 45° rotation
 * (output must be a rotated quadrant pattern), writes results as raw NV12
 * files for host-side visual check.
 *
 * PASS criteria (automatic):
 *   - identity dewarp: max pixel delta vs input <= 2 (bilinear resampling)
 *
 * Usage: hal-dsp-rotate-dewarp-test [width=640] [height=480]
 * Output: /tmp/dsp_ident.nv12 (identity dewarp), /tmp/dsp_rot45.nv12 (rotate)
 */

#include "common/hal_common.h"
#include "dsp/hal_dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

uint32_t g_w = 640, g_h = 480;

void make_nv12(std::vector<uint8_t> &buf, uint32_t w, uint32_t h)
{
    buf.assign(static_cast<size_t>(w) * h + (w / 2) * (h / 2) * 2, 0);
    uint8_t *y = buf.data();
    uint8_t *uv = y + static_cast<size_t>(w) * h;
    /* Four quadrants with distinct luma + chroma:
     * TL: bright/cold  TR: bright/warm  BL: dark/neutral  BR: mid/green */
    for (uint32_t j = 0; j < h; ++j)
    {
        const bool top = j < h / 2;
        for (uint32_t i = 0; i < w; ++i)
        {
            const bool left = i < w / 2;
            y[j * w + i] = top ? (left ? 220 : 180) : (left ? 40 : 120);
        }
    }
    for (uint32_t j = 0; j < h / 2; ++j)
    {
        for (uint32_t i = 0; i < w / 2; ++i)
        {
            const bool top = j < h / 4;
            const bool left = i < w / 4;
            const int u = top ? (left ? 40 : 120) : (left ? 128 : 80);
            const int v = top ? (left ? 200 : 220) : (left ? 128 : 40);
            uv[(j * (w / 2) + i) * 2 + 0] = static_cast<uint8_t>(u);
            uv[(j * (w / 2) + i) * 2 + 1] = static_cast<uint8_t>(v);
        }
    }
}

void fill_frame(HalFrameBuffer *f, uint8_t *base, uint32_t w, uint32_t h)
{
    std::memset(f, 0, sizeof(*f));
    f->width = w;
    f->height = h;
    f->format = HAL_PIX_FMT_NV12;
    f->num_planes = 2;
    f->mem_type = HAL_MEM_MALLOC;
    f->planes[0] = base;
    f->sizes[0] = static_cast<size_t>(w) * h;
    f->strides[0] = w;
    f->planes[1] = base + static_cast<size_t>(w) * h;
    f->sizes[1] = (w / 2) * (h / 2) * 2;
    f->strides[1] = w;
}

bool save_file(const char *path, const uint8_t *data, size_t len)
{
    FILE *f = std::fopen(path, "wb");
    if (!f)
    {
        return false;
    }
    std::fwrite(data, 1, len, f);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc >= 2) g_w = static_cast<uint32_t>(std::atoi(argv[1]));
    if (argc >= 3) g_h = static_cast<uint32_t>(std::atoi(argv[2]));
    if (g_w % 2 || g_h % 2)
    {
        std::fprintf(stderr, "even dimensions required\n");
        return 2;
    }

    void *dsp = nullptr;
    HalDspConfig dcfg{};
    int rc = HAL_DSP_OPS.init(&dcfg, &dsp);
    if (rc != HAL_OK || !dsp)
    {
        std::fprintf(stderr, "HAL_DSP_OPS.init failed rc=%d\n", rc);
        return 1;
    }

    std::vector<uint8_t> src, dst;
    make_nv12(src, g_w, g_h);
    dst.resize(src.size());

    HalFrameBuffer sf{}, df{};
    fill_frame(&sf, src.data(), g_w, g_h);
    fill_frame(&df, dst.data(), g_w, g_h);

    /* ---- 1. Identity-mesh dewarp (automatic PASS check) ---- */
    {
        /* DSP cell size is 64x64: grid must cover the output image. */
        const uint32_t cols = (g_w + 63U) / 64U + 1U;
        const uint32_t rows = (g_h + 63U) / 64U + 1U;
        std::vector<float> mesh(static_cast<size_t>(cols) * rows * 2U);
        for (uint32_t r = 0; r < rows; ++r)
        {
            for (uint32_t c = 0; c < cols; ++c)
            {
                /* output grid point -> same source coordinate (identity) */
                const float x = static_cast<float>(std::min<uint32_t>(c * 64U, g_w - 1U));
                const float y = static_cast<float>(std::min<uint32_t>(r * 64U, g_h - 1U));
                mesh[(static_cast<size_t>(r) * cols + c) * 2U + 0U] = x;
                mesh[(static_cast<size_t>(r) * cols + c) * 2U + 1U] = y;
            }
        }
        HalDspDewarpParams p{};
        p.src = &sf;
        p.dst = &df;
        p.mesh_xy = mesh.data();
        p.grid_cols = cols;
        p.grid_rows = rows;
        p.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
        rc = HAL_DSP_OPS.dewarp ? HAL_DSP_OPS.dewarp(dsp, &p) : HAL_ERR_NOT_SUPPORTED;
        std::printf("dewarp(identity) ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        if (rc == HAL_OK)
        {
            uint32_t maxd = 0;
            double sum = 0;
            const size_t n = static_cast<size_t>(g_w) * g_h;
            for (size_t i = 0; i < n; ++i)
            {
                const int d = std::abs(static_cast<int>(dst[i]) - static_cast<int>(src[i]));
                maxd = std::max<uint32_t>(maxd, static_cast<uint32_t>(d));
                sum += d;
            }
            (void)save_file("/tmp/dsp_ident.nv12", dst.data(), dst.size());
            std::printf("  identity dewarp: max_delta=%u avg_delta=%.3f -> %s\n", maxd, sum / n,
                        maxd <= 2U ? "PASS" : "FAIL");
        }
    }

    /* ---- 2. Arbitrary-angle rotation (45°, visual output) ---- */
    {
        std::vector<uint8_t> rot(src.size());
        HalFrameBuffer rf{};
        fill_frame(&rf, rot.data(), g_w, g_h);
        HalDspRotateParams p{};
        p.src = &sf;
        p.dst = &rf;
        p.angle_deg_cw = 45.0f;
        p.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
        rc = HAL_DSP_OPS.rotate ? HAL_DSP_OPS.rotate(dsp, &p) : HAL_ERR_NOT_SUPPORTED;
        std::printf("rotate(45) ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        if (rc == HAL_OK)
        {
            /* Non-degenerate output check: rotated frame must have varied luma. */
            uint32_t minv = 255, maxv = 0;
            for (size_t i = 0; i < static_cast<size_t>(g_w) * g_h; ++i)
            {
                minv = std::min<uint32_t>(minv, rot[i]);
                maxv = std::max<uint32_t>(maxv, rot[i]);
            }
            (void)save_file("/tmp/dsp_rot45.nv12", rot.data(), rot.size());
            std::printf("  rotate(45): luma range [%u..%u] -> %s\n", minv, maxv,
                        (maxv - minv) > 100U ? "PASS (visually check /tmp/dsp_rot45.nv12)" : "SUSPECT");
        }
    }

    /* ---- 3. Telescopic multi-crop (large downscale) ---- */
    {
        std::vector<uint8_t> out(static_cast<size_t>(80) * 60 + 40 * 30 * 2);
        HalFrameBuffer of{};
        fill_frame(&of, out.data(), 80, 60);
        HalDspMultiCropOutput mout{};
        mout.crop.start_x = 0;
        mout.crop.start_y = 0;
        mout.crop.end_x = static_cast<int32_t>(g_w);
        mout.crop.end_y = static_cast<int32_t>(g_h);
        mout.dst = &of;
        mout.scaling_mode = HAL_DSP_SCALING_STRETCH;
        HalDspMultiCropResizeParams p{};
        p.src = &sf;
        p.outputs = &mout;
        p.output_count = 1;
        p.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
        rc = HAL_DSP_OPS.multi_crop_resize_telescopic ? HAL_DSP_OPS.multi_crop_resize_telescopic(dsp, &p)
                                                      : HAL_ERR_NOT_SUPPORTED;
        std::printf("telescopic(640->80) ret=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        if (rc == HAL_OK)
        {
            uint32_t minv = 255, maxv = 0;
            for (size_t i = 0; i < static_cast<size_t>(80) * 60; ++i)
            {
                minv = std::min<uint32_t>(minv, out[i]);
                maxv = std::max<uint32_t>(maxv, out[i]);
            }
            std::printf("  telescopic: luma range [%u..%u]\n", minv, maxv);
        }
    }

    /* ---- 4. Telescopic multi-crop via DMABUF (real dma-buf fd path) ---- */
    {
        HalFrameBufferRequest req{};
        req.width = 80;
        req.height = 60;
        req.format = HAL_PIX_FMT_NV12;
        req.mem_type = HAL_MEM_DMABUF;
        req.zero_initialize = true;
        HalFrameBuffer *of = nullptr;
        int arc = HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &of);
        std::printf("request_frame_buffer(DMABUF 80x60) rc=%d fd=%d\n", arc,
                    (of && of->dma_fds && of->dma_fds[0] >= 0) ? of->dma_fds[0] : -1);
        if (arc == HAL_OK && of)
        {
            HalDspMultiCropOutput mout{};
            mout.crop.start_x = 0;
            mout.crop.start_y = 0;
            mout.crop.end_x = static_cast<int32_t>(g_w);
            mout.crop.end_y = static_cast<int32_t>(g_h);
            mout.dst = of;
            mout.scaling_mode = HAL_DSP_SCALING_STRETCH;
            HalDspMultiCropResizeParams p{};
            p.src = &sf;
            p.outputs = &mout;
            p.output_count = 1;
            p.interpolation = HAL_DSP_INTERPOLATION_BILINEAR;
            /* repeat 5x to expose the userptr-style random page-layout failures */
            int ok = 0, fail = 0;
            for (int i = 0; i < 5; ++i)
            {
                const int rc = HAL_DSP_OPS.multi_crop_resize_telescopic
                    ? HAL_DSP_OPS.multi_crop_resize_telescopic(dsp, &p) : HAL_ERR_NOT_SUPPORTED;
                if (rc == HAL_OK) ++ok; else ++fail;
            }
            std::printf("telescopic DMABUF: %d/5 ok -> %s\n", ok, (fail == 0) ? "PASS" : "FLAKY");
            (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(of);
        }
    }

    (void)HAL_DSP_OPS.deinit(dsp);
    std::printf("done.\n");
    return 0;
}
