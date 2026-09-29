/**
 * @file dmabuf_infer_test.cpp
 * @brief Compare NPU input paths: DMA-BUF fd direct binding (zero-copy) vs
 *        CPU copy (tensor_from_frame memcpy + userptr submit).
 *
 * Allocates a DMABUF frame matching the model's first input stream (geometry
 * from get_model_info; RGB888/NV12 orders), then runs N synchronous run()
 * iterations per mode:
 *   - "copy" : tensor_from_frame() -> CPU copy into a malloc tensor, bound
 *              via MemoryView (userptr; driver builds a scatter list)
 *   - "fd"   : tensor_from_frame_ex() fast path -> borrows the frame's
 *              dma-buf fd, bound via set_dma_buffer (no CPU copy)
 * Both modes read the identical zero-initialized memory, so the first output
 * tensor must be byte-identical — correctness is checked alongside timing.
 *
 * Usage: hal-dmabuf-infer-test <model.hef> [iters=200]
 */

#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

double now_ms()
{
    const auto t = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(t.time_since_epoch()).count();
}

struct ModeResult
{
    int rc = -1;
    double avg_ms = 0.0;
    double min_ms = 1e9;
    double max_ms = 0.0;
    uint32_t out_bytes = 0;
    std::vector<uint8_t> out0;
};

/* Run `iters` synchronous inferences with a pre-built input tensor. */
ModeResult run_mode(HalInferenceSession *sess, const HalTensor &in, HalModelInfo &mi, int iters)
{
    ModeResult r;
    const HalTensor inputs[1] = {in};
    HalTensor outs[4]{};
    const int out_count = mi.num_outputs < 4u ? static_cast<int>(mi.num_outputs) : 4;

    /* warmup */
    for (int i = 0; i < 10; ++i)
    {
        const int rc = HAL_INFERENCE_OPS.run(sess, inputs, 1, outs, out_count);
        if (rc != HAL_OK)
        {
            r.rc = rc;
            std::printf("  run() rc=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
            return r;
        }
        for (int k = 0; k < out_count; ++k)
        {
            (void)HAL_INFERENCE_OPS.free_tensor(&outs[k]);
        }
    }

    double total = 0.0;
    for (int i = 0; i < iters; ++i)
    {
        std::memset(outs, 0, sizeof(outs));
        const double t0 = now_ms();
        const int rc = HAL_INFERENCE_OPS.run(sess, inputs, 1, outs, out_count);
        const double dt = now_ms() - t0;
        if (rc != HAL_OK)
        {
            r.rc = rc;
            std::printf("  run() iter %d rc=%d (%s)\n", i, rc, hal_error_to_string((HalErrorCode)rc));
            return r;
        }
        total += dt;
        if (dt < r.min_ms) r.min_ms = dt;
        if (dt > r.max_ms) r.max_ms = dt;
        if (i == 0)
        {
            /* capture first output for the cross-mode equality check */
            if (outs[0].data && outs[0].byte_size > 0)
            {
                r.out0.assign(static_cast<const uint8_t *>(outs[0].data),
                              static_cast<const uint8_t *>(outs[0].data) + outs[0].byte_size);
                r.out_bytes = outs[0].byte_size;
            }
        }
        for (int k = 0; k < out_count; ++k)
        {
            (void)HAL_INFERENCE_OPS.free_tensor(&outs[k]);
        }
    }
    r.avg_ms = total / iters;
    r.rc = HAL_OK;
    return r;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <model.hef> [iters=200]\n", argv[0]);
        return 2;
    }
    const int iters = (argc >= 3) ? std::atoi(argv[2]) : 200;

    HalInferenceConfig cfg{};
    std::snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", argv[1]);
    cfg.timeout_ms = 10000;
    HalInferenceSession *sess = HAL_INFERENCE_OPS.create(&cfg);
    if (!sess)
    {
        std::fprintf(stderr, "create() failed\n");
        return 1;
    }
    HalModelInfo mi{};
    int rc = HAL_INFERENCE_OPS.get_model_info(sess, &mi);
    if (rc != HAL_OK || mi.num_inputs == 0 || mi.num_outputs == 0)
    {
        std::fprintf(stderr, "get_model_info rc=%d\n", rc);
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 1;
    }
    const HalModelTensorInfo &in0 = mi.inputs[0];
    /* NHWC image tensors: shape[1]=H, shape[2]=W (see hal_inference.h). */
    const int32_t h = in0.ndim >= 3 ? in0.shape[in0.ndim - 3] : 0;
    const int32_t w = in0.ndim >= 2 ? in0.shape[in0.ndim - 2] : 0;
    std::printf("model=%s input=%s ndim=%d shape=[%d,%d,%d,%d] byte_size=%u dtype=%d\n", argv[1], in0.name,
                in0.ndim, in0.shape[0], in0.shape[1], in0.shape[2], in0.shape[3], in0.byte_size, (int)in0.dtype);
    if (w <= 0 || h <= 0)
    {
        std::fprintf(stderr, "cannot derive input geometry from model info\n");
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 1;
    }
    /* The zero-copy fast path matches RGB24 frames; NV12-order models also
     * work (single dmabuf holds both planes). Choose from the input dtype. */
    const HalPixelFormat fmt = (in0.dtype == HAL_DTYPE_UINT8 && in0.ndim == 4 && in0.shape[3] == 1)
                                   ? HAL_PIX_FMT_NV12   /* 1-feature uint8 streams are NV12 models */
                                   : HAL_PIX_FMT_RGB24;

    /* ---- Shared DMABUF frame (exact match to the model input) ---- */
    HalFrameBufferRequest req{};
    req.width = static_cast<uint32_t>(w);
    req.height = static_cast<uint32_t>(h);
    req.format = fmt;
    req.mem_type = HAL_MEM_DMABUF;
    req.zero_initialize = true; /* zeroed + cache-synced for device reads */
    HalFrameBuffer *fb = nullptr;
    rc = HAL_FRAME_BUFFER_OPS.request_frame_buffer(&req, &fb);
    if (rc != HAL_OK || !fb)
    {
        std::fprintf(stderr, "request_frame_buffer(DMABUF %dx%d) rc=%d (%s)\n", w, h, rc,
                     hal_error_to_string((HalErrorCode)rc));
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 1;
    }
    std::printf("frame: DMABUF %dx%d fmt=%d fd=%d planes=%u\n", fb->width, fb->height, (int)fb->format,
                fb->dma_fds[0], fb->num_planes);
    if (fb->dma_fds[0] < 0)
    {
        std::fprintf(stderr, "frame has no dma fd\n");
        (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(fb);
        (void)HAL_INFERENCE_OPS.destroy(sess);
        return 1;
    }

    /* ---- Mode A ("copy"): legacy tensor_from_frame -> CPU copy ---- */
    ModeResult ma;
    {
        HalTensor in{};
        rc = HAL_INFERENCE_OPS.tensor_from_frame(fb, &in);
        if (rc != HAL_OK)
        {
            std::printf("copy mode: tensor_from_frame rc=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        }
        else
        {
            ma = run_mode(sess, in, mi, iters);
            (void)HAL_INFERENCE_OPS.free_tensor(&in);
        }
    }

    /* ---- Mode B ("fd"): _ex fast path -> borrowed dma-buf ---- */
    ModeResult mb;
    {
        HalTensor in{};
        rc = HAL_INFERENCE_OPS.tensor_from_frame_ex ? HAL_INFERENCE_OPS.tensor_from_frame_ex(sess, fb, &in)
                                                    : HAL_ERR_NOT_SUPPORTED;
        if (rc != HAL_OK)
        {
            std::printf("fd mode: tensor_from_frame_ex rc=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        }
        else if (in.dma_fd < 0)
        {
            std::printf("fd mode: tensor carries no dma_fd (fast path not taken)\n");
            (void)HAL_INFERENCE_OPS.free_tensor(&in);
        }
        else
        {
            std::printf("fd mode: zero-copy tensor fd=%d bytes=%u\n", in.dma_fd, in.byte_size);
            mb = run_mode(sess, in, mi, iters);
            (void)HAL_INFERENCE_OPS.free_tensor(&in);
        }
    }

    std::printf("\n===== results (%d iters each) =====\n", iters);
    if (ma.rc == HAL_OK)
    {
        std::printf("copy: avg %.3f ms  min %.3f  max %.3f  -> %.1f fps\n", ma.avg_ms, ma.min_ms, ma.max_ms,
                    1000.0 / ma.avg_ms);
    }
    else
    {
        std::printf("copy: FAILED rc=%d\n", ma.rc);
    }
    if (mb.rc == HAL_OK)
    {
        std::printf("fd  : avg %.3f ms  min %.3f  max %.3f  -> %.1f fps\n", mb.avg_ms, mb.min_ms, mb.max_ms,
                    1000.0 / mb.avg_ms);
    }
    else
    {
        std::printf("fd  : FAILED rc=%d\n", mb.rc);
    }
    if (ma.rc == HAL_OK && mb.rc == HAL_OK)
    {
        if (ma.avg_ms > 0 && mb.avg_ms > 0)
        {
            std::printf("delta: fd saves %.3f ms/run (%.1f%%)\n", ma.avg_ms - mb.avg_ms,
                        100.0 * (ma.avg_ms - mb.avg_ms) / ma.avg_ms);
        }
        const bool same = (ma.out0.size() == mb.out0.size()) &&
                          (ma.out0.empty() || std::memcmp(ma.out0.data(), mb.out0.data(), ma.out0.size()) == 0);
        std::printf("output equality (first tensor, %u bytes): %s\n", ma.out_bytes, same ? "MATCH" : "MISMATCH");
    }

    (void)HAL_FRAME_BUFFER_OPS.release_frame_buffer(fb);
    (void)HAL_INFERENCE_OPS.destroy(sess);
    return 0;
}
