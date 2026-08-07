/**
 * @file hal_draw_hailo15.cpp
 * @brief Hailo-15 HAL_DRAW_OPS using HailoNV12Mat (aligns with V1 hal_ml_overlay_hailo15.cpp).
 *
 * Draws on NV12 Y/UV via hailo_postprocess_tools — no full-frame BGR conversion. When
 * @c HAL_HAVE_HAILO_POSTPROCESS_TOOLS is unset or frame wrap fails, falls back to @c hal_draw_cpu_draw_*.
 */

#include "common/hal_common.h"
#include "common/hal_buffer.h"
#include "model/hal_draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

#include <sys/mman.h>
#include <unistd.h>

#ifdef HAL_HAVE_HAILO_POSTPROCESS_TOOLS
/* Vendor SDK headers: suppress noisy warnings we cannot fix upstream. */
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wignored-qualifiers"
#pragma clang diagnostic ignored "-Wunused-parameter"
#pragma clang diagnostic ignored "-Wextra"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#pragma GCC diagnostic ignored "-Wignored-qualifiers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#include "hailo_postprocess_tools/image_utils/hailomat.hpp"
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#endif

extern "C" {
extern int hal_draw_cpu_draw_detection(const HalDetectionResult *r, HalFrameBuffer *f,
                                       const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_classification(const HalClassificationResult *r, HalFrameBuffer *f,
                                            const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_segmentation(const HalSegmentationResult *r, HalFrameBuffer *f,
                                          const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_keypoint(const HalKeypointResult *r, HalFrameBuffer *f,
                                      const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_result(const HalPostprocessResult *r, HalFrameBuffer *f,
                                    const HalDrawConfig *cfg);
extern int hal_draw_cpu_draw_rect(HalFrameBuffer *f, const HalDrawRect *r);
extern int hal_draw_cpu_draw_line(HalFrameBuffer *f, const HalDrawLine *l);
extern int hal_draw_cpu_draw_circle(HalFrameBuffer *f, const HalDrawCircle *c);
extern int hal_draw_cpu_draw_text(HalFrameBuffer *f, const HalDrawText *t);
extern int hal_draw_cpu_draw_polygon(HalFrameBuffer *f, const HalDrawPolygon *p);
extern int hal_draw_cpu_draw_mask(HalFrameBuffer *f, const HalDrawMask *m);
extern int hal_draw_cpu_draw_mosaic(HalFrameBuffer *f, const HalDrawMosaic *m);
}

#ifdef HAL_HAVE_HAILO_POSTPROCESS_TOOLS

namespace hailo15_draw_impl
{

struct MappedPlane {
    void *ptr;
    size_t length;
};

static std::mutex g_dma_map_mutex;
static std::unordered_map<int, MappedPlane> g_dma_mapped;

static void *map_dma_fd_cached(int fd, size_t length)
{
    if (fd < 0 || length == 0)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_dma_map_mutex);
    auto it = g_dma_mapped.find(fd);
    if (it != g_dma_mapped.end())
        return it->second.ptr;
    void *addr = mmap(nullptr, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (addr == MAP_FAILED)
        return nullptr;
    g_dma_mapped.emplace(fd, MappedPlane{addr, length});
    return addr;
}

/** HailoMat uses cv::Scalar as (R,G,B), not OpenCV BGR. */
static cv::Scalar hal_color_to_rgb_scalar(const HalColor &c)
{
    return cv::Scalar(c.r, c.g, c.b);
}

static cv::Rect clamp_rect(cv::Rect r, int max_w, int max_h)
{
    int x1 = std::max(0, r.x);
    int y1 = std::max(0, r.y);
    int x2 = std::min(max_w, r.x + r.width);
    int y2 = std::min(max_h, r.y + r.height);
    return cv::Rect(x1, y1, std::max(0, x2 - x1), std::max(0, y2 - y1));
}

static std::unique_ptr<HailoMat> make_hailo_mat_from_hal_v2_frame(HalFrameBuffer *frame)
{
    if (!frame)
        return nullptr;
    const uint32_t width = frame->width;
    const uint32_t height = frame->height;
    if (width == 0 || height == 0)
        return nullptr;

    switch (frame->format)
    {
    case HAL_PIX_FMT_NV12: {
        uint8_t *y_plane = static_cast<uint8_t *>(frame->planes[0]);
        uint8_t *uv_plane = (frame->num_planes >= 2) ? static_cast<uint8_t *>(frame->planes[1]) : nullptr;

        if (frame->mem_type == HAL_MEM_DMABUF)
        {
            if (!y_plane && frame->dma_fds[0] >= 0 && frame->sizes[0] > 0)
            {
                void *mapped = map_dma_fd_cached(frame->dma_fds[0], frame->sizes[0]);
                if (!mapped)
                    return nullptr;
                y_plane = static_cast<uint8_t *>(mapped);
                frame->planes[0] = y_plane;
            }
            if (!uv_plane && frame->num_planes >= 2 && frame->dma_fds[1] >= 0 && frame->sizes[1] > 0)
            {
                void *mapped = map_dma_fd_cached(frame->dma_fds[1], frame->sizes[1]);
                if (!mapped)
                    return nullptr;
                uv_plane = static_cast<uint8_t *>(mapped);
                frame->planes[1] = uv_plane;
            }
        }

        if (!y_plane || frame->num_planes < 2 || !uv_plane)
            return nullptr;

        const uint32_t y_stride = frame->strides[0] ? frame->strides[0] : width;
        const uint32_t uv_stride = frame->strides[1] ? frame->strides[1] : width;
        return std::make_unique<HailoNV12Mat>(nullptr, height, width, y_stride, uv_stride, 1, 1, y_plane,
                                              uv_plane);
    }
    default:
        return nullptr;
    }
}

static float draw_auto_font_scale(const HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    if (!f || !cfg)
        return 2.0f;
    const float h = (float)std::max(1u, f->height);
    return std::max(2.0f, cfg->default_font_scale * (h / 480.0f));
}

static bool class_id_allowed(int32_t class_id, const HalDrawConfig *cfg)
{
    if (!cfg || cfg->num_class_ids_filter == 0)
        return true;
    for (uint32_t i = 0; i < cfg->num_class_ids_filter; i++)
    {
        if (cfg->class_ids_filter[i] == class_id)
            return true;
    }
    return false;
}

static int draw_keypoint_hailomat(const HalKeypointResult *kp, HalFrameBuffer *f, const HalDrawConfig *cfg,
                                  HailoMat *hmat, HailoNV12Mat *nv12);

static int draw_result_hailomat(const HalPostprocessResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_result(r, f, cfg);

    auto *nv12 = dynamic_cast<HailoNV12Mat *>(hmat.get());
    if (!nv12)
        return hal_draw_cpu_draw_result(r, f, cfg);

    const int fw = (int)f->width;
    const int fh = (int)f->height;

    if ((r->type == HAL_POST_TYPE_DETECTION || r->type == HAL_POST_TYPE_OCR_DETECTION) && cfg->draw_detections)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        const uint32_t thick = cfg->default_box_thickness ? (uint32_t)cfg->default_box_thickness : 2u;
        const cv::Scalar box_col = hal_color_to_rgb_scalar(cfg->default_box_color);

        for (uint32_t i = 0; i < r->result.detection.num_detections; i++)
        {
            const HalDetection &d = r->result.detection.detections[i];
            HalDrawRect hr{};
            hal_bbox_to_rect(&d.bbox, f->width, f->height, &hr);
            cv::Rect rect(hr.x, hr.y, hr.width, hr.height);
            rect = clamp_rect(rect, fw, fh);
            if (rect.width <= 0 || rect.height <= 0)
                continue;

            for (uint32_t t = 0; t < thick; t++)
            {
                cv::Rect tr = clamp_rect(
                    cv::Rect(rect.x - (int)(t / 2), rect.y - (int)(t / 2), rect.width + (int)t, rect.height + (int)t),
                    fw, fh);
                if (tr.width > 0 && tr.height > 0)
                    nv12->draw_rectangle_opencv(tr, box_col);
            }

            if (cfg->draw_detection_labels)
            {
                char buf[HAL_MAX_TEXT_LEN];
                const bool want_conf = cfg->draw_detection_confidence;
                if (d.label[0] != '\0')
                {
                    if (want_conf)
                        std::snprintf(buf, sizeof(buf), "%s %.2f", d.label, (double)d.confidence);
                    else
                        std::snprintf(buf, sizeof(buf), "%s", d.label);
                }
                else
                {
                    if (want_conf)
                        std::snprintf(buf, sizeof(buf), "%d %.2f", (int)d.class_id, (double)d.confidence);
                    else
                        std::snprintf(buf, sizeof(buf), "%d", (int)d.class_id);
                }
                const double font_scale = std::max(0.4, (double)fs * 0.2);
                cv::Point pt(hr.x, std::max(0, hr.y - (int)std::lround(10.f * fs)));
                hmat->draw_text(std::string(buf), pt, font_scale, hal_color_to_rgb_scalar(cfg->default_text_color));
            }
        }
        return HAL_OK;
    }

    if (r->type == HAL_POST_TYPE_CLIP)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        const uint32_t max_show = (cfg->clip_max_lines > 0) ? cfg->clip_max_lines : 5U;
        const uint32_t n = std::min(r->result.classification.num_classes, max_show);
        int y = (int)(10.f * fs);
        for (uint32_t i = 0; i < n; i++)
        {
            const HalClassification &c = r->result.classification.classes[i];
            char line[HAL_MAX_TEXT_LEN];
            if (c.label[0] != '\0')
                std::snprintf(line, sizeof(line), "%s: %.3f", c.label, (double)c.confidence);
            else
                std::snprintf(line, sizeof(line), "%d: %.3f", (int)c.class_id, (double)c.confidence);
            const double font_scale = std::max(0.4, (double)fs * 0.2);
            hmat->draw_text(std::string(line), cv::Point((int)(10.f * fs), y), font_scale,
                            hal_color_to_rgb_scalar(cfg->default_text_color));
            y += (int)(10.f * fs);
        }
        return HAL_OK;
    }

    if (r->type == HAL_POST_TYPE_SEGMENTATION)
        return hal_draw_cpu_draw_segmentation(&r->result.segmentation, f, cfg);

    if (r->type == HAL_POST_TYPE_KEYPOINT)
        return draw_keypoint_hailomat(&r->result.keypoint, f, cfg, hmat.get(), nv12);

    if (r->type == HAL_POST_TYPE_CLASSIFICATION && cfg->draw_classifications)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        int y = (int)std::lround(10.f * fs);
        for (uint32_t i = 0; i < r->result.classification.num_classes && i < HAL_MAX_CLASSES; i++)
        {
            const HalClassification &c = r->result.classification.classes[i];
            char line[HAL_MAX_TEXT_LEN];
            if (c.label[0] != '\0')
                std::snprintf(line, sizeof(line), "%s: %.3f", c.label, (double)c.confidence);
            else
                std::snprintf(line, sizeof(line), "%d: %.3f", (int)c.class_id, (double)c.confidence);
            const double font_scale = std::max(0.4, (double)fs * 0.2);
            hmat->draw_text(std::string(line), cv::Point((int)std::lround(10.f * fs), y), font_scale,
                            hal_color_to_rgb_scalar(cfg->default_text_color));
            y += (int)std::lround(10.f * fs);
        }
        return HAL_OK;
    }

    if (r->type == HAL_POST_TYPE_OCR_RECOGNITION && cfg->draw_ocr)
    {
        const float fs = draw_auto_font_scale(f, cfg);
        const uint32_t thick = cfg->default_box_thickness ? (uint32_t)cfg->default_box_thickness : 2u;
        const cv::Scalar box_col = hal_color_to_rgb_scalar(cfg->default_box_color);
        for (uint32_t i = 0; i < r->result.ocr.num_lines; i++)
        {
            const HalOcrLine &ln = r->result.ocr.lines[i];
            HalDrawRect hr{};
            hal_bbox_to_rect(&ln.bbox, f->width, f->height, &hr);
            cv::Rect rect(hr.x, hr.y, hr.width, hr.height);
            rect = clamp_rect(rect, fw, fh);
            if (rect.width <= 0 || rect.height <= 0)
                continue;
            for (uint32_t t = 0; t < thick; t++)
            {
                cv::Rect tr = clamp_rect(
                    cv::Rect(rect.x - (int)(t / 2), rect.y - (int)(t / 2), rect.width + (int)t, rect.height + (int)t),
                    fw, fh);
                if (tr.width > 0 && tr.height > 0)
                    nv12->draw_rectangle_opencv(tr, box_col);
            }
            char buf[HAL_MAX_TEXT_LEN];
            if (ln.text[0] != '\0')
            {
                if (cfg->draw_detection_confidence)
                    std::snprintf(buf, sizeof(buf), "%s %.2f", ln.text, (double)ln.confidence);
                else
                    std::snprintf(buf, sizeof(buf), "%s", ln.text);
            }
            else if (cfg->draw_detection_confidence)
                std::snprintf(buf, sizeof(buf), "%.2f", (double)ln.confidence);
            else
                buf[0] = '\0';
            if (buf[0] != '\0')
            {
                const double font_scale = std::max(0.4, (double)fs * 0.2);
                cv::Point pt(hr.x, std::max(0, hr.y - (int)std::lround(10.f * fs)));
                hmat->draw_text(std::string(buf), pt, font_scale, hal_color_to_rgb_scalar(cfg->default_text_color));
            }
        }
        return HAL_OK;
    }

    /* Depth colormap: CPU NV12 blend (same helper as hal_draw_cpu draw_result). */
    if (r->type == HAL_POST_TYPE_DEPTH)
        return hal_draw_cpu_draw_result(r, f, cfg);

    return HAL_ERR_NOT_SUPPORTED;
}

static int draw_keypoint_hailomat(const HalKeypointResult *kp, HalFrameBuffer *f, const HalDrawConfig *cfg,
                                  HailoMat *hmat, HailoNV12Mat *nv12)
{
    if (!kp || !cfg || !hmat || !nv12)
        return hal_draw_cpu_draw_keypoint(kp, f, cfg);

    const int fw = (int)f->width;
    const int fh = (int)f->height;
    const int kp_r = cfg->default_keypoint_radius > 0 ? cfg->default_keypoint_radius : 3;

    for (uint32_t oi = 0; oi < kp->num_objects; oi++)
    {
        const HalKeypointObject &obj = kp->objects[oi];
        if (!class_id_allowed(obj.class_id, cfg))
            continue;

        if (obj.bbox.w > 0.0f && obj.bbox.h > 0.0f)
        {
            HalDrawRect hr{};
            hal_bbox_to_rect(&obj.bbox, f->width, f->height, &hr);
            cv::Rect rect(hr.x, hr.y, hr.width, hr.height);
            rect = clamp_rect(rect, fw, fh);
            if (rect.width > 0 && rect.height > 0)
            {
                const uint32_t thick = cfg->default_box_thickness ? (uint32_t)cfg->default_box_thickness : 2u;
                const cv::Scalar col = hal_color_to_rgb_scalar(cfg->default_box_color);
                for (uint32_t t = 0; t < thick; t++)
                {
                    cv::Rect tr = clamp_rect(
                        cv::Rect(rect.x - (int)(t / 2), rect.y - (int)(t / 2), rect.width + (int)t,
                                 rect.height + (int)t),
                        fw, fh);
                    if (tr.width > 0 && tr.height > 0)
                        nv12->draw_rectangle_opencv(tr, col);
                }
            }
        }

        if (cfg->draw_skeleton && kp->num_links > 0)
        {
            for (uint32_t li = 0; li < kp->num_links; li++)
            {
                const HalSkeletonLink &link = kp->links[li];
                if (link.from_idx < 0 || link.to_idx < 0)
                    continue;
                if (static_cast<uint32_t>(link.from_idx) >= obj.num_keypoints ||
                    static_cast<uint32_t>(link.to_idx) >= obj.num_keypoints)
                    continue;
                const HalPoint2D &a = obj.keypoints[link.from_idx];
                const HalPoint2D &b = obj.keypoints[link.to_idx];
                if (a.confidence <= 0.0f || b.confidence <= 0.0f)
                    continue;
                int ax, ay, bx, by;
                hal_point_to_pixel(&a, f->width, f->height, &ax, &ay);
                hal_point_to_pixel(&b, f->width, f->height, &bx, &by);
                const int tk = link.thickness > 0.0f ? (int)std::lround(link.thickness) : 2;
                hmat->draw_line(cv::Point(ax, ay), cv::Point(bx, by), hal_color_to_rgb_scalar(link.color),
                                std::max(1, tk), cv::LINE_8);
            }
        }

        if (cfg->draw_keypoints)
        {
            for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
            {
                const HalPoint2D &pt = obj.keypoints[ki];
                if (pt.confidence <= 0.0f)
                    continue;
                HalColor kcol = cfg->default_keypoint_color;
                for (uint32_t si = 0; si < cfg->num_class_styles; si++)
                {
                    if (cfg->class_styles[si].class_id == obj.class_id)
                    {
                        kcol = cfg->class_styles[si].keypoint_color;
                        break;
                    }
                }
                int px, py;
                hal_point_to_pixel(&pt, f->width, f->height, &px, &py);
                cv::Scalar c = hal_color_to_rgb_scalar(kcol);
                hmat->draw_ellipse(cv::Point(px, py), cv::Size(kp_r, kp_r), 0.0, 0.0, 360.0, c, -1);
            }
        }

        if (cfg->draw_skeleton && kp->num_links == 0 && cfg->draw_keypoints && obj.num_keypoints > 1)
        {
            int prev_x = 0, prev_y = 0;
            bool have_prev = false;
            for (uint32_t ki = 0; ki < obj.num_keypoints; ki++)
            {
                const HalPoint2D &pt = obj.keypoints[ki];
                if (pt.confidence <= 0.0f)
                {
                    have_prev = false;
                    continue;
                }
                int px, py;
                hal_point_to_pixel(&pt, f->width, f->height, &px, &py);
                if (have_prev)
                    hmat->draw_line(cv::Point(prev_x, prev_y), cv::Point(px, py),
                                    hal_color_to_rgb_scalar(cfg->default_keypoint_color), 2, cv::LINE_8);
                prev_x = px;
                prev_y = py;
                have_prev = true;
            }
        }
    }
    return HAL_OK;
}

static int hailo_draw_rect(HalFrameBuffer *f, const HalDrawRect *r)
{
    if (!f || !r)
        return HAL_ERR_INVALID_ARG;
    if (r->thickness < 0)
        return hal_draw_cpu_draw_rect(f, r);

    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    auto *nv12 = hmat ? dynamic_cast<HailoNV12Mat *>(hmat.get()) : nullptr;
    if (!nv12)
        return hal_draw_cpu_draw_rect(f, r);

    const int fw = (int)f->width;
    const int fh = (int)f->height;
    cv::Rect rect(r->x, r->y, r->width, r->height);
    rect = clamp_rect(rect, fw, fh);
    if (rect.width <= 0 || rect.height <= 0)
        return HAL_OK;
    const cv::Scalar col = hal_color_to_rgb_scalar(r->color);
    const int tmax = std::max(1, r->thickness);
    for (int t = 0; t < tmax; t++)
    {
        cv::Rect tr = clamp_rect(
            cv::Rect(rect.x - t / 2, rect.y - t / 2, rect.width + t, rect.height + t), fw, fh);
        if (tr.width > 0 && tr.height > 0)
            nv12->draw_rectangle_opencv(tr, col);
    }
    return HAL_OK;
}

static int hailo_draw_line(HalFrameBuffer *f, const HalDrawLine *l)
{
    if (!f || !l)
        return HAL_ERR_INVALID_ARG;
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_line(f, l);
    hmat->draw_line(cv::Point(l->x1, l->y1), cv::Point(l->x2, l->y2), hal_color_to_rgb_scalar(l->color),
                    std::max(1, l->thickness), cv::LINE_8);
    return HAL_OK;
}

static int hailo_draw_circle(HalFrameBuffer *f, const HalDrawCircle *c)
{
    if (!f || !c || c->radius <= 0)
        return HAL_ERR_INVALID_ARG;
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_circle(f, c);
    const int tk = c->thickness < 0 ? -1 : std::max(1, c->thickness);
    hmat->draw_ellipse(cv::Point(c->x, c->y), cv::Size(c->radius, c->radius), 0.0, 0.0, 360.0,
                       hal_color_to_rgb_scalar(c->color), tk);
    return HAL_OK;
}

static int hailo_draw_text(HalFrameBuffer *f, const HalDrawText *t)
{
    if (!f || !t)
        return HAL_ERR_INVALID_ARG;
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_text(f, t);

    const double font_scale = std::max(0.3, (double)t->font_scale * 0.2);
    const cv::Scalar fg = hal_color_to_rgb_scalar(t->color);
    const cv::Scalar bg = hal_color_to_rgb_scalar(t->bg_color);

    auto &mats = hmat->get_matrices();
    if (!mats.empty() && t->bg_color.a > 0 && (bg[0] > 0 || bg[1] > 0 || bg[2] > 0))
    {
        int baseline = 0;
        cv::Size sz = cv::getTextSize(t->text, cv::FONT_HERSHEY_SIMPLEX, font_scale, std::max(1, t->thickness),
                                      &baseline);
        cv::Rect bg_rect = clamp_rect(cv::Rect(t->x, t->y - sz.height, sz.width, sz.height + baseline),
                                        mats[0].cols, mats[0].rows);
        if (bg_rect.width > 0 && bg_rect.height > 0)
            cv::rectangle(mats[0], bg_rect, bg, cv::FILLED);
    }
    hmat->draw_text(std::string(t->text), cv::Point(t->x, t->y), font_scale, fg);
    return HAL_OK;
}

static int hailo_draw_polygon(HalFrameBuffer *f, const HalDrawPolygon *p)
{
    if (!f || !p)
        return HAL_ERR_INVALID_ARG;
    if (p->thickness < 0)
        return hal_draw_cpu_draw_polygon(f, p);

    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_polygon(f, p);

    const cv::Scalar col = hal_color_to_rgb_scalar(p->color);
    const int tk = (p->thickness == 0) ? 1 : p->thickness;
    for (uint32_t i = 0; i < p->num_points; i++)
    {
        const uint32_t j = (i + 1) % p->num_points;
        hmat->draw_line(cv::Point(p->points_x[i], p->points_y[i]), cv::Point(p->points_x[j], p->points_y[j]), col,
                        std::max(1, tk), cv::LINE_8);
    }
    return HAL_OK;
}

// RGB → BT.601 YUV. Mirrors hal_draw_cpu's rgb_to_yuv so the CPU and Hailo
// draw paths produce identical colors for the same HalColor input.
static inline void hailo_rgb_to_yuv(uint8_t r, uint8_t g, uint8_t b, uint8_t &y, uint8_t &u, uint8_t &v)
{
    const int yi = (66 * r + 129 * g + 25 * b + 128) >> 8;
    const int ui = (-38 * r - 74 * g + 112 * b + 128) >> 8;
    const int vi = (112 * r - 94 * g - 18 * b + 128) >> 8;
    auto cl = [](int vv) -> uint8_t {
        return vv < 0 ? 0 : (vv > 255 ? 255 : static_cast<uint8_t>(vv));
    };
    y = cl(yi + 16);
    u = cl(ui + 128);
    v = cl(vi + 128);
}

// Mask overlay: alpha-blend a solid color onto NV12 Y + UV, gated by a bytemask.
// DMABUF-safe — make_hailo_mat_from_hal_v2_frame() mmaps the dma_fd and writes
// the CPU pointers back into frame->planes[], so the raw-plane access below is
// valid for both HAL_MEM_MMAP and HAL_MEM_DMABUF frames (the CPU fallback
// hal_draw_cpu_draw_mask bails on DMABUF because frame_is_nv12() requires
// non-NULL planes before mapping happens). UV is blended too; a luma-only
// overlay would read as a flat gray box and never show the chosen color.
static int hailo_draw_mask(HalFrameBuffer *f, const HalDrawMask *m)
{
    if (!f || !m || !m->mask_data)
        return HAL_ERR_INVALID_ARG;

    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    auto *nv12 = hmat ? dynamic_cast<HailoNV12Mat *>(hmat.get()) : nullptr;
    if (!nv12 || f->format != HAL_PIX_FMT_NV12 || f->num_planes < 2 || !f->planes[0] || !f->planes[1])
        return hal_draw_cpu_draw_mask(f, m);

    auto &mats = hmat->get_matrices();
    if (mats.empty())
        return hal_draw_cpu_draw_mask(f, m);

    const int fw = (int)f->width;
    const int fh = (int)f->height;
    const int x0 = std::max(0, m->x);
    const int y0 = std::max(0, m->y);
    const int x1 = std::min(fw, m->x + (int)m->width);
    const int y1 = std::min(fh, m->y + (int)m->height);
    if (x0 >= x1 || y0 >= y1)
        return HAL_OK;

    float alpha = m->alpha;
    if (alpha < 0.0f) alpha = 0.0f;
    if (alpha > 1.0f) alpha = 1.0f;
    if (alpha <= 0.0f)
        return HAL_OK;

    uint8_t Yc, Uc, Vc;
    hailo_rgb_to_yuv(m->color.r, m->color.g, m->color.b, Yc, Uc, Vc);

    cv::Rect rect = clamp_rect(cv::Rect(x0, y0, x1 - x0, y1 - y0), mats[0].cols, mats[0].rows);
    if (rect.width <= 0 || rect.height <= 0)
        return HAL_OK;

    // Bytemask window over the rect. mask_data spans the full mask
    // (m->width × m->height at offset m->x,m->y); the rect is a sub-window.
    const int mask_stride = (int)m->width;
    const int mx = x0 - m->x;
    const int my = y0 - m->y;
    cv::Mat mask_roi(rect.height, rect.width, CV_8UC1,
                     const_cast<uint8_t *>(m->mask_data + my * mask_stride + mx), mask_stride);

    // Y: blended = Y*(1-a) + Yc*a, written only where the mask is set.
    cv::Mat roi_y = mats[0](rect);
    cv::Mat blended_y;
    roi_y.convertTo(blended_y, CV_8U, (1.0 - alpha), (double)Yc * alpha);
    blended_y.copyTo(roi_y, mask_roi);

    // UV: interleaved (U,V), subsampled 2×2. Even-aligned rect (frontend does
    // x/y/w/h &= ~1) gives clean 2×2 blocks.
    const int uv_stride = f->strides[1] ? (int)f->strides[1] : (int)f->width;
    const int uv_cols = uv_stride / 2;
    const int uv_rows = (int)f->height / 2;
    const int uv_x0 = rect.x / 2;
    const int uv_y0 = rect.y / 2;
    const int uv_w = rect.width / 2;
    const int uv_h = rect.height / 2;
    if (uv_cols <= 0 || uv_rows <= 0 || uv_w <= 0 || uv_h <= 0 || uv_x0 + uv_w > uv_cols ||
        uv_y0 + uv_h > uv_rows)
        return HAL_OK; // Y already applied; UV out of range — nothing more to do.

    cv::Mat uv_full(uv_rows, uv_cols, CV_8UC2, f->planes[1], uv_stride);
    cv::Mat uv_roi = uv_full(cv::Rect(uv_x0, uv_y0, uv_w, uv_h));

    // Downsample the Y bytemask to UV resolution: a chroma sample is masked if
    // any of its 2×2 luma samples is masked.
    static thread_local cv::Mat uv_mask;
    uv_mask.create(uv_h, uv_w, CV_8UC1);
    for (int uy = 0; uy < uv_h; ++uy)
    {
        uint8_t *out = uv_mask.ptr<uint8_t>(uy);
        for (int ux = 0; ux < uv_w; ++ux)
        {
            uint8_t vset = 0;
            for (int dy = 0; dy < 2 && !vset; ++dy)
            {
                const int ly = uy * 2 + dy;
                if (ly >= rect.height)
                    break;
                const uint8_t *mrow = mask_roi.ptr<uint8_t>(ly);
                for (int dx = 0; dx < 2; ++dx)
                {
                    const int lx = ux * 2 + dx;
                    if (lx >= rect.width)
                        break;
                    if (mrow[lx])
                    {
                        vset = 255;
                        break;
                    }
                }
            }
            out[ux] = vset;
        }
    }

    // U,V blended = src*(1-a) + (Uc,Vc)*a, applied via the downsampled mask.
    static thread_local cv::Mat overlay_uv;
    overlay_uv.create(uv_roi.size(), CV_8UC2);
    overlay_uv.setTo(cv::Scalar(Uc, Vc));
    cv::Mat blended_uv;
    cv::addWeighted(uv_roi, (1.0 - alpha), overlay_uv, alpha, 0.0, blended_uv);
    blended_uv.copyTo(uv_roi, uv_mask);

    return HAL_OK;
}

static int hailo_draw_mosaic(HalFrameBuffer *f, const HalDrawMosaic *m)
{
    if (!f || !m)
        return HAL_ERR_INVALID_ARG;
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    if (!hmat)
        return hal_draw_cpu_draw_mosaic(f, m);

    auto &mats = hmat->get_matrices();
    if (mats.empty())
        return hal_draw_cpu_draw_mosaic(f, m);

    const int width = (int)f->width;
    const int height = (int)f->height;
    const int x0 = std::max(0, m->x);
    const int y0 = std::max(0, m->y);
    const int x1 = std::min(width, m->x + (int)m->width);
    const int y1 = std::min(height, m->y + (int)m->height);
    if (x0 >= x1 || y0 >= y1)
        return HAL_OK;

    cv::Rect rect = clamp_rect(cv::Rect(x0, y0, x1 - x0, y1 - y0), mats[0].cols, mats[0].rows);
    if (rect.width <= 0 || rect.height <= 0)
        return HAL_OK;

    cv::Mat roi = mats[0](rect);

    // UV plane (subsampled 2×2) — processed in BOTH modes so chroma follows
    // luma. Without UV, blur leaves sharp color detail behind and mosaic
    // leaves sharp chroma at block edges. make_hailo_mat_from_hal_v2_frame()
    // above already mapped DMABUF and wrote CPU pointers into f->planes[].
    const int uv_stride = f->strides[1] ? (int)f->strides[1] : (int)f->width;
    const int uv_cols = uv_stride / 2;
    const int uv_rows = (int)f->height / 2;
    const int uv_x0 = rect.x / 2;
    const int uv_y0 = rect.y / 2;
    const int uv_w = rect.width / 2;
    const int uv_h = rect.height / 2;
    const bool have_uv = (uv_cols > 0 && uv_rows > 0 && uv_w > 0 && uv_h > 0 &&
                          uv_x0 + uv_w <= uv_cols && uv_y0 + uv_h <= uv_rows);
    cv::Mat uv_full, uv_roi;
    if (have_uv)
    {
        uv_full = cv::Mat(uv_rows, uv_cols, CV_8UC2, f->planes[1], uv_stride);
        uv_roi = uv_full(cv::Rect(uv_x0, uv_y0, uv_w, uv_h));
    }

    if (m->block_size <= 0)
    {
        // Blur. The old 5×5 luma-only Gaussian was invisible on a 1080p frame.
        // Use a pyramid down/up-sample (INTER_AREA → INTER_LINEAR): a true box
        // average whose cost is O(px), not O(k·px), so it stays strong and
        // affordable for any ROI size. Down-scale factor scales with the ROI.
        auto pyramid_blur = [](cv::Mat region) {
            const int min_dim = std::min(region.cols, region.rows);
            const int s = std::max(2, std::min(min_dim / 4, 40));
            cv::Size small(std::max(2, region.cols / s), std::max(2, region.rows / s));
            cv::Mat tmp, out;
            cv::resize(region, tmp, small, 0, 0, cv::INTER_AREA);
            cv::resize(tmp, out, region.size(), 0, 0, cv::INTER_LINEAR);
            out.copyTo(region);
        };
        pyramid_blur(roi);
        if (have_uv)
            pyramid_blur(uv_roi);
    }
    else
    {
        const uint32_t block = m->block_size ? m->block_size : 16u;
        const int small_w = std::max(1, rect.width / (int)block);
        const int small_h = std::max(1, rect.height / (int)block);
        cv::Mat tmp;
        cv::resize(roi, tmp, cv::Size(small_w, small_h), 0, 0, cv::INTER_LINEAR);
        cv::resize(tmp, roi, roi.size(), 0, 0, cv::INTER_NEAREST);
        if (have_uv)
        {
            const int us_w = std::max(1, uv_w / (int)block);
            const int us_h = std::max(1, uv_h / (int)block);
            cv::Mat tmp_uv;
            cv::resize(uv_roi, tmp_uv, cv::Size(us_w, us_h), 0, 0, cv::INTER_LINEAR);
            cv::resize(tmp_uv, uv_roi, uv_roi.size(), 0, 0, cv::INTER_NEAREST);
        }
    }
    return HAL_OK;
}

static int hailo_draw_detection(const HalDetectionResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalPostprocessResult pr{};
    pr.type = HAL_POST_TYPE_DETECTION;
    pr.result.detection = *r;
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    return draw_result_hailomat(&pr, f, c);
}

static int hailo_draw_classification(const HalClassificationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalPostprocessResult pr{};
    pr.type = HAL_POST_TYPE_CLASSIFICATION;
    pr.result.classification = *r;
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    return draw_result_hailomat(&pr, f, c);
}

static int hailo_draw_segmentation(const HalSegmentationResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    return hal_draw_cpu_draw_segmentation(r, f, cfg);
}

static int hailo_draw_keypoint(const HalKeypointResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    auto hmat = make_hailo_mat_from_hal_v2_frame(f);
    auto *nv12 = hmat ? dynamic_cast<HailoNV12Mat *>(hmat.get()) : nullptr;
    if (!hmat || !nv12)
        return hal_draw_cpu_draw_keypoint(r, f, c);
    return draw_keypoint_hailomat(r, f, c, hmat.get(), nv12);
}

static int hailo_draw_result(const HalPostprocessResult *r, HalFrameBuffer *f, const HalDrawConfig *cfg)
{
    HalDrawConfig local{};
    const HalDrawConfig *c = cfg;
    if (!c)
    {
        hal_draw_config_init_default(&local);
        c = &local;
    }
    return draw_result_hailomat(r, f, c);
}

static const char *hailo_draw_version(void)
{
    return "HAL-DRAW Hailo15 (HailoNV12Mat + OpenCV-on-NV12 planes; aligns with V1 hal_ml_overlay)";
}

} // namespace hailo15_draw_impl

#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */

extern "C" {
#ifdef HAL_HAVE_HAILO_POSTPROCESS_TOOLS
HalDrawOps HAL_DRAW_OPS = {
    .draw_detection_result = hailo15_draw_impl::hailo_draw_detection,
    .draw_classification_result = hailo15_draw_impl::hailo_draw_classification,
    .draw_segmentation_result = hailo15_draw_impl::hailo_draw_segmentation,
    .draw_keypoint_result = hailo15_draw_impl::hailo_draw_keypoint,
    .draw_result = hailo15_draw_impl::hailo_draw_result,
    .draw_rect = hailo15_draw_impl::hailo_draw_rect,
    .draw_line = hailo15_draw_impl::hailo_draw_line,
    .draw_circle = hailo15_draw_impl::hailo_draw_circle,
    .draw_text = hailo15_draw_impl::hailo_draw_text,
    .draw_polygon = hailo15_draw_impl::hailo_draw_polygon,
    .draw_mask = hailo15_draw_impl::hailo_draw_mask,
    .draw_mosaic = hailo15_draw_impl::hailo_draw_mosaic,
    .get_version = hailo15_draw_impl::hailo_draw_version,
};
#else /* !HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
static const char *hailo_draw_version_cpu_fallback(void)
{
    return "HAL-DRAW Hailo15 (CPU rasterizer: install hailo_postprocess_tools + OpenCV for HailoNV12Mat path)";
}

HalDrawOps HAL_DRAW_OPS = {
    .draw_detection_result = hal_draw_cpu_draw_detection,
    .draw_classification_result = hal_draw_cpu_draw_classification,
    .draw_segmentation_result = hal_draw_cpu_draw_segmentation,
    .draw_keypoint_result = hal_draw_cpu_draw_keypoint,
    .draw_result = hal_draw_cpu_draw_result,
    .draw_rect = hal_draw_cpu_draw_rect,
    .draw_line = hal_draw_cpu_draw_line,
    .draw_circle = hal_draw_cpu_draw_circle,
    .draw_text = hal_draw_cpu_draw_text,
    .draw_polygon = hal_draw_cpu_draw_polygon,
    .draw_mask = hal_draw_cpu_draw_mask,
    .draw_mosaic = hal_draw_cpu_draw_mosaic,
    .get_version = hailo_draw_version_cpu_fallback,
};
#endif /* HAL_HAVE_HAILO_POSTPROCESS_TOOLS */
} /* extern "C" */
