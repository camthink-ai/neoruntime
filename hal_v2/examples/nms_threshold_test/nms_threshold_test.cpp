/**
 * @file nms_threshold_test.cpp
 * @brief Verify HalInferenceConfig.nms + tensor_from_frame_ex + decode_nms.
 *
 * Loads a detection HEF twice — once with a low NMS score threshold, once with
 * a high one — runs both on the same NV12 frame through the session-aware
 * tensor_from_frame_ex (letterbox + NV12->RGB handled by the HAL) and decodes
 * the on-chip NMS output with hal_inference_decode_nms.
 *
 * Expected: low threshold yields >= high threshold detections.
 *
 * Usage: hal-nms-threshold-test <model.hef> <frame> <w> <h> [nv12|rgb|bgr]
 */

#include "common/hal_buffer.h"
#include "common/hal_common.h"
#include "model/hal_inference.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace
{

struct RunResult
{
    int rc = -1;
    uint32_t boxes = 0;
    HalNmsDetection best{};
};

static HalPixelFormat g_fmt = HAL_PIX_FMT_NV12;

RunResult run_with_threshold(const char *hef, const std::vector<uint8_t> &nv12, uint32_t w, uint32_t h,
                             float score_thr)
{
    RunResult r;
    HalInferenceConfig cfg{};
    std::snprintf(cfg.model_path, sizeof(cfg.model_path), "%s", hef);
    cfg.use_dma = false;
    cfg.timeout_ms = 10000;
    /* Preprocess rules — applied by tensor_from_frame_ex. */
    cfg.preprocess.color = HAL_PREPROCESS_COLOR_NONE;
    cfg.preprocess.resize = HAL_PREPROCESS_RESIZE_BILINEAR;
    cfg.preprocess.letterbox = HAL_PREPROCESS_LETTERBOX_KEEP_ASPECT;
    cfg.preprocess.pad_value = 114;
    cfg.nms.score_threshold = score_thr;

    HalInferenceSession *sess = HAL_INFERENCE_OPS.create(&cfg);
    if (!sess)
    {
        std::printf("  create() failed (threshold=%.3f)\n", score_thr);
        return r;
    }
    HalModelInfo mi{};
    int rc = HAL_INFERENCE_OPS.get_model_info(sess, &mi);
    if (rc != HAL_OK || mi.num_inputs == 0 || mi.num_outputs == 0)
    {
        std::printf("  get_model_info rc=%d\n", rc);
        (void)HAL_INFERENCE_OPS.destroy(sess);
        r.rc = rc ? rc : HAL_ERR_INVALID_STATE;
        return r;
    }

    HalFrameBuffer frame{};
    frame.width = w;
    frame.height = h;
    frame.format = g_fmt;
    frame.mem_type = HAL_MEM_MALLOC;
    if (g_fmt == HAL_PIX_FMT_NV12)
    {
        frame.num_planes = 2;
        frame.planes[0] = const_cast<uint8_t *>(nv12.data());
        frame.sizes[0] = static_cast<size_t>(w) * h;
        frame.strides[0] = w;
        frame.planes[1] = const_cast<uint8_t *>(nv12.data()) + static_cast<size_t>(w) * h;
        frame.sizes[1] = (w / 2) * (h / 2) * 2;
        frame.strides[1] = w;
    }
    else
    {
        frame.num_planes = 1;
        frame.planes[0] = const_cast<uint8_t *>(nv12.data());
        frame.sizes[0] = static_cast<size_t>(w) * h * 3;
        frame.strides[0] = w * 3;
    }

    /* The HAL now owns geometry/format adaptation. */
    HalTensor in{};
    rc = HAL_INFERENCE_OPS.tensor_from_frame_ex
             ? HAL_INFERENCE_OPS.tensor_from_frame_ex(sess, &frame, &in)
             : HAL_ERR_NOT_SUPPORTED;
    if (rc != HAL_OK)
    {
        std::printf("  tensor_from_frame_ex rc=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
        (void)HAL_INFERENCE_OPS.destroy(sess);
        r.rc = rc;
        return r;
    }

    const HalTensor inputs[1] = {in};
    HalTensor outs[4]{};
    const int out_count = mi.num_outputs < 4u ? static_cast<int>(mi.num_outputs) : 4;
    rc = HAL_INFERENCE_OPS.run(sess, inputs, 1, outs, out_count);
    if (rc == HAL_OK)
    {
        HalNmsDetection dets[64]{};
        uint32_t n = 0;
        const int drc = hal_inference_decode_nms(&outs[0], dets, 64, &n);
        if (drc == HAL_OK)
        {
            r.boxes = n;
            float best = -1.0f;
            for (uint32_t i = 0; i < n; ++i)
            {
                if (dets[i].score > best)
                {
                    best = dets[i].score;
                    r.best = dets[i];
                }
            }
        }
        else
        {
            std::printf("  decode_nms rc=%d (%s)\n", drc, hal_error_to_string((HalErrorCode)drc));
        }
        for (int i = 0; i < out_count; ++i)
        {
            (void)HAL_INFERENCE_OPS.free_tensor(&outs[i]);
        }
    }
    else
    {
        std::printf("  run rc=%d (%s)\n", rc, hal_error_to_string((HalErrorCode)rc));
    }
    (void)HAL_INFERENCE_OPS.free_tensor(&in);
    (void)HAL_INFERENCE_OPS.destroy(sess);
    r.rc = rc;
    return r;
}

} // namespace

int main(int argc, char **argv)
{
    if (argc < 5)
    {
        std::fprintf(stderr, "usage: %s <model.hef> <frame.nv12> <w> <h>\n", argv[0]);
        return 2;
    }
    FILE *f = std::fopen(argv[2], "rb");
    if (!f)
    {
        std::fprintf(stderr, "cannot open %s\n", argv[2]);
        return 2;
    }
    const uint32_t w = static_cast<uint32_t>(std::atoi(argv[3]));
    const uint32_t h = static_cast<uint32_t>(std::atoi(argv[4]));
    if (argc >= 6)
    {
        if (std::strcmp(argv[5], "rgb") == 0) g_fmt = HAL_PIX_FMT_RGB24;
        else if (std::strcmp(argv[5], "bgr") == 0) g_fmt = HAL_PIX_FMT_BGR24;
        else g_fmt = HAL_PIX_FMT_NV12;
    }
    const size_t want = (g_fmt == HAL_PIX_FMT_NV12)
        ? (static_cast<size_t>(w) * h + (w / 2) * (h / 2) * 2)
        : (static_cast<size_t>(w) * h * 3);
    std::vector<uint8_t> nv12(want);
    if (std::fread(nv12.data(), 1, nv12.size(), f) != nv12.size())
    {
        std::fprintf(stderr, "short read on %s\n", argv[2]);
        std::fclose(f);
        return 2;
    }
    std::fclose(f);

    std::printf("model=%s frame(fmt=%d)=%ux%u\n", argv[1], (int)g_fmt, w, h);
    const RunResult low = run_with_threshold(argv[1], nv12, w, h, 0.001f);
    std::printf("  [low  thr=0.001] rc=%d boxes=%u best(score=%.3f box=[%.2f,%.2f,%.2f,%.2f])\n", low.rc,
                low.boxes, low.best.score, low.best.x_min, low.best.y_min, low.best.x_max, low.best.y_max);
    const RunResult high = run_with_threshold(argv[1], nv12, w, h, 0.60f);
    std::printf("  [high thr=0.600] rc=%d boxes=%u best(score=%.3f)\n", high.rc, high.boxes, high.best.score);

    if (low.rc == HAL_OK && high.rc == HAL_OK)
    {
        std::printf("verdict: low>=high boxes: %s (%u vs %u)\n",
                    low.boxes >= high.boxes ? "PASS" : "FAIL", low.boxes, high.boxes);
        return (low.boxes >= high.boxes) ? 0 : 1;
    }
    return 1;
}
