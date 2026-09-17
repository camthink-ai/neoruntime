// Unit tests for the AI overlay face-blur geometry: label matching,
// normalized bbox → pixel mosaic rect scaling, frame-bound clipping,
// and cap handling in collect_face_mosaic_rects().

#include "ai_overlay_subscriber.h"

#include <cassert>
#include <cstring>

static HalPostprocessResult make_detection_result(const char* const labels[],
                                                  size_t n,
                                                  const float bboxes[][4]) {
    HalPostprocessResult r{};
    r.type = HAL_POST_TYPE_DETECTION;
    auto& det = r.result.detection;
    det.num_detections = static_cast<uint32_t>(n);
    for (size_t i = 0; i < n; i++) {
        strncpy(det.detections[i].label, labels[i], sizeof(det.detections[i].label) - 1);
        det.detections[i].bbox.x = bboxes[i][0];
        det.detections[i].bbox.y = bboxes[i][1];
        det.detections[i].bbox.w = bboxes[i][2];
        det.detections[i].bbox.h = bboxes[i][3];
    }
    return r;
}

int main() {
    const uint32_t FW = 1920, FH = 1080;

    /* ---- label matching: exact "face" in any case; substrings rejected ---- */
    {
        const char* labels[] = {"face", "Face", "FACE", "person", "facial", "face_mask", ""};
        // Non-degenerate bboxes: zero-size rects are skipped by contract, so
        // all-zero boxes would turn this into a geometry test, not a label test.
        float bboxes[7][4] = {{0.10f, 0.10f, 0.20f, 0.20f},
                              {0.35f, 0.10f, 0.20f, 0.20f},
                              {0.60f, 0.10f, 0.20f, 0.20f},
                              {0.10f, 0.40f, 0.20f, 0.20f},
                              {0.35f, 0.40f, 0.20f, 0.20f},
                              {0.60f, 0.40f, 0.20f, 0.20f},
                              {0.10f, 0.70f, 0.20f, 0.20f}};
        auto r = make_detection_result(labels, 7, bboxes);
        HalDrawMosaic out[7] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, 7) == 3);
        assert(out[0].x == 192 && out[1].x == 672 && out[2].x == 1152);  // the three faces, in order
        assert(out[0].block_size == 8 && out[1].block_size == 8 && out[2].block_size == 8);
    }

    /* ---- normalized bbox scaled to pixel coordinates ---- */
    {
        const char* labels[] = {"face"};
        float bboxes[1][4] = {{0.25f, 0.5f, 0.5f, 0.25f}};
        auto r = make_detection_result(labels, 1, bboxes);
        HalDrawMosaic out[1] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, 1) == 1);
        assert(out[0].x == 480);         // 0.25 * 1920
        assert(out[0].y == 540);         // 0.50 * 1080
        assert(out[0].width == 960);     // 0.50 * 1920
        assert(out[0].height == 270);    // 0.25 * 1080
        assert(out[0].block_size == 8);
    }

    /* ---- bbox overflowing the frame is clipped at the frame edge ---- */
    {
        const char* labels[] = {"face"};
        float bboxes[1][4] = {{0.9f, 0.9f, 0.3f, 0.3f}};
        auto r = make_detection_result(labels, 1, bboxes);
        HalDrawMosaic out[1] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 0, out, 1) == 1);
        assert(out[0].x == 1728);
        assert(out[0].y == 972);
        assert(out[0].width == (int32_t)FW - 1728);   // 192
        assert(out[0].height == (int32_t)FH - 972);   // 108
        assert(out[0].block_size == 0);               // 0 = blur, passed through
    }

    /* ---- bbox fully outside the frame yields a zero-size rect → skipped ---- */
    {
        const char* labels[] = {"face"};
        float bboxes[1][4] = {{1.2f, 0.1f, 0.5f, 0.2f}};  // x sanitized to 1.0
        auto r = make_detection_result(labels, 1, bboxes);
        HalDrawMosaic out[1] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, 1) == 0);
    }

    /* ---- cap: more faces than the output array holds ---- */
    {
        const char* labels[] = {"face", "face", "face"};
        float bboxes[3][4] = {{0.0f, 0.0f, 0.1f, 0.1f},
                              {0.2f, 0.0f, 0.1f, 0.1f},
                              {0.4f, 0.0f, 0.1f, 0.1f}};
        auto r = make_detection_result(labels, 3, bboxes);
        HalDrawMosaic out[2] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, 2) == 2);
        assert(out[0].x == 0 && out[1].x == 384);  // first two, in order
    }

    /* ---- guards: zero frame dims, zero cap, non-detection result ---- */
    {
        const char* labels[] = {"face"};
        float bboxes[1][4] = {{0.25f, 0.5f, 0.5f, 0.25f}};
        auto r = make_detection_result(labels, 1, bboxes);
        HalDrawMosaic out[1] = {};
        assert(collect_face_mosaic_rects(r, 0, 0, 8, out, 1) == 0);
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, 0) == 0);
        assert(collect_face_mosaic_rects(r, FW, FH, 8, nullptr, 4) == 0);

        HalPostprocessResult cls{};
        cls.type = HAL_POST_TYPE_CLASSIFICATION;
        assert(collect_face_mosaic_rects(cls, FW, FH, 8, out, 1) == 0);
    }

    /* ---- oversized num_detections (malformed payload) is clamped to the array ---- */
    {
        const char* labels[] = {"face"};
        float bboxes[1][4] = {{0.25f, 0.5f, 0.5f, 0.25f}};
        auto r = make_detection_result(labels, 1, bboxes);
        r.result.detection.num_detections = HAL_MAX_DETECTIONS + 100;
        HalDrawMosaic out[HAL_MAX_DETECTIONS] = {};
        assert(collect_face_mosaic_rects(r, FW, FH, 8, out, HAL_MAX_DETECTIONS) == 1);
    }

    return 0;
}
