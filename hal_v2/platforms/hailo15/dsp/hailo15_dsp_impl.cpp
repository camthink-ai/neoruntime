#include "hailo15_dsp_priv.hpp"

#include <array>
#include <cstring>

extern "C" {

static int hailo15_dsp_convert_format_sync(Hailo15DspContext *ctx, const HalDspConvertFormatParams *params);
static int hailo15_dsp_resize_sync(Hailo15DspContext *ctx, const HalDspResizeParams *params);
static int hailo15_dsp_crop_resize_sync(Hailo15DspContext *ctx, const HalDspCropResizeParams *params);
static int hailo15_dsp_multi_crop_resize_sync(Hailo15DspContext *ctx, const HalDspMultiCropResizeParams *params);
static int hailo15_dsp_blend_sync(Hailo15DspContext *ctx, const HalDspBlendParams *params);
static int hailo15_dsp_flip_rotate_sync(Hailo15DspContext *ctx, const HalDspFlipRotateParams *params);
static int hailo15_dsp_privacy_mask_sync(Hailo15DspContext *ctx, const HalDspPrivacyMaskParams *params);

/* ---------------------- Helpers ---------------------- */

static HalErrorCode dsp_status_to_hal(dsp_status status)
{
    if (DSP_SUCCESS == status) {
        return HAL_OK;
    }
    return HAL_ERR_RESULT;
}

static dsp_image_format_t hal_pixfmt_to_dsp_format(HalPixelFormat fmt)
{
    switch (fmt) {
    case HAL_PIX_FMT_NV12:   return DSP_IMAGE_FORMAT_NV12;
    case HAL_PIX_FMT_RGB24:  return DSP_IMAGE_FORMAT_RGB;
    case HAL_PIX_FMT_BGR24:  return DSP_IMAGE_FORMAT_BGR;
    case HAL_PIX_FMT_ARGB32: return DSP_IMAGE_FORMAT_ARGB;
    case HAL_PIX_FMT_GRAY8:  return DSP_IMAGE_FORMAT_GRAY8;
    default:
        break;
    }
    return DSP_IMAGE_FORMAT_NV12;
}

static dsp_interpolation_type_t hal_interp_to_dsp(HalDspInterpolation interp)
{
    switch (interp) {
    case HAL_DSP_INTERPOLATION_NEAREST:  return INTERPOLATION_TYPE_NEAREST_NEIGHBOR;
    case HAL_DSP_INTERPOLATION_AREA:     return INTERPOLATION_TYPE_AREA;
    case HAL_DSP_INTERPOLATION_BICUBIC:  return INTERPOLATION_TYPE_BICUBIC;
    case HAL_DSP_INTERPOLATION_BILINEAR:
    default:
        return INTERPOLATION_TYPE_BILINEAR;
    }
}

static dsp_scaling_mode_t hal_scaling_to_dsp(HalDspScalingMode mode)
{
    switch (mode) {
    case HAL_DSP_SCALING_STRETCH:            return DSP_SCALING_MODE_STRETCH;
    case HAL_DSP_SCALING_LETTERBOX_MIDDLE:   return DSP_SCALING_MODE_LETTERBOX_MIDDLE;
    case HAL_DSP_SCALING_LETTERBOX_UP_LEFT:  return DSP_SCALING_MODE_LETTERBOX_UP_LEFT;
    case HAL_DSP_SCALING_SCALE_AND_CROP:     return DSP_SCALING_MODE_SCALE_AND_CROP;
    default:                                 return DSP_SCALING_MODE_STRETCH;
    }
}

static dsp_letterbox_alignment_t hal_letterbox_to_dsp(HalDspLetterboxAlignment a)
{
    switch (a) {
    case HAL_DSP_LETTERBOX_MIDDLE:  return DSP_LETTERBOX_MIDDLE;
    case HAL_DSP_LETTERBOX_UP_LEFT: return DSP_LETTERBOX_UP_LEFT;
    case HAL_DSP_LETTERBOX_NONE:
    default:
        return DSP_NO_LETTERBOX;
    }
}

static void hal_color_to_dsp(const HalDspColor *src, dsp_color_t *dst)
{
    if (!src || !dst) {
        return;
    }
    dst->r = src->r;
    dst->g = src->g;
    dst->b = src->b;
}

static void hal_frame_to_dsp_image(const HalFrameBuffer *frame,
                                   dsp_image_properties_t *image,
                                   dsp_data_plane_t *planes_storage,
                                   size_t planes_storage_count)
{
    std::memset(image, 0, sizeof(*image));
    image->width  = frame->width;
    image->height = frame->height;
    image->format = hal_pixfmt_to_dsp_format(frame->format);
    image->planes_count = frame->num_planes;

    image->planes = planes_storage;

    const uint32_t n = (frame->num_planes > planes_storage_count) ? (uint32_t)planes_storage_count : frame->num_planes;
    for (uint32_t i = 0; i < n; ++i) {
        if (frame->mem_type == HAL_MEM_DMABUF && frame->dma_fds[i] >= 0) {
            planes_storage[i].fd = frame->dma_fds[i];
        } else {
            planes_storage[i].userptr = frame->planes[i];
        }
        planes_storage[i].bytesperline = frame->strides[i];
        planes_storage[i].bytesused    = frame->sizes[i];
    }

    image->memory = (frame->mem_type == HAL_MEM_DMABUF) ? DSP_MEMORY_TYPE_DMABUF
                                                        : DSP_MEMORY_TYPE_USERPTR;
}

/* ---------------------- Worker thread ---------------------- */

static void hailo15_dsp_worker_thread(Hailo15DspContext *ctx)
{
    while (!ctx->stop_flag.load()) {
        Hailo15DspJobItem item{};
        {
            std::unique_lock<std::mutex> lock(ctx->queue_mtx);
            ctx->queue_cv.wait(lock, [&] {
                return ctx->stop_flag.load() || !ctx->job_queue.empty();
            });
            if (ctx->stop_flag.load()) {
                break;
            }
            item = ctx->job_queue.front();
            ctx->job_queue.pop();
        }

        HalDspJobHandle job = item.job;
        if (!job) {
            continue;
        }

        int rc = HAL_ERR_INVALID_ARG;
        switch (job->op_type) {
        case HAL_DSP_OP_CONVERT_FORMAT:
            rc = hailo15_dsp_convert_format_sync(
                ctx, static_cast<const HalDspConvertFormatParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_RESIZE:
            rc = hailo15_dsp_resize_sync(
                ctx, static_cast<const HalDspResizeParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_CROP_RESIZE:
            rc = hailo15_dsp_crop_resize_sync(
                ctx, static_cast<const HalDspCropResizeParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_MULTI_CROP_RESIZE:
            rc = hailo15_dsp_multi_crop_resize_sync(
                ctx, static_cast<const HalDspMultiCropResizeParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_BLEND:
            rc = hailo15_dsp_blend_sync(
                ctx, static_cast<const HalDspBlendParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_FLIP_ROTATE:
            rc = hailo15_dsp_flip_rotate_sync(
                ctx, static_cast<const HalDspFlipRotateParams *>(job->params_copy));
            break;
        case HAL_DSP_OP_PRIVACY_MASK:
            rc = hailo15_dsp_privacy_mask_sync(
                ctx, static_cast<const HalDspPrivacyMaskParams *>(job->params_copy));
            break;
        default:
            rc = HAL_ERR_NOT_SUPPORTED;
            break;
        }

        {
            std::lock_guard<std::mutex> guard(job->mtx);
            job->result.status = (rc == HAL_OK) ? HAL_DSP_JOB_COMPLETED : HAL_DSP_JOB_FAILED;
            job->result.result_code = rc;
            job->completed.store(true);
        }
        job->cv.notify_all();
    }
}

/* ---------------------- Sync operations ---------------------- */

static int hailo15_dsp_convert_format_sync(Hailo15DspContext *ctx, const HalDspConvertFormatParams *params)
{
    (void)ctx;
    dsp_image_properties_t src_image{};
    dsp_image_properties_t dst_image{};
    dsp_data_plane_t src_planes[HAL_MAX_PLANES]{};
    dsp_data_plane_t dst_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->src, &src_image, src_planes, HAL_MAX_PLANES);
    hal_frame_to_dsp_image(params->dst, &dst_image, dst_planes, HAL_MAX_PLANES);
    dsp_status st = dsp_convert_format(ctx->device, &src_image, &dst_image);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_resize_sync(Hailo15DspContext *ctx, const HalDspResizeParams *params)
{
    (void)ctx;
    if (!params || !params->src || !params->dst)
    {
        return HAL_ERR_INVALID_ARG;
    }
    dsp_resize_params_t r{};
    dsp_image_properties_t src_image{};
    dsp_image_properties_t dst_image{};
    dsp_data_plane_t src_planes[HAL_MAX_PLANES]{};
    dsp_data_plane_t dst_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->src, &src_image, src_planes, HAL_MAX_PLANES);
    hal_frame_to_dsp_image(params->dst, &dst_image, dst_planes, HAL_MAX_PLANES);
    r.src = &src_image;
    r.dst = &dst_image;
    r.interpolation = hal_interp_to_dsp(params->interpolation);
    dsp_status st = dsp_resize(ctx->device, &r);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_crop_resize_sync(Hailo15DspContext *ctx, const HalDspCropResizeParams *params)
{
    (void)ctx;
    dsp_resize_params_t r{};
    dsp_image_properties_t src_image{};
    dsp_image_properties_t dst_image{};
    dsp_data_plane_t src_planes[HAL_MAX_PLANES]{};
    dsp_data_plane_t dst_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->src, &src_image, src_planes, HAL_MAX_PLANES);
    hal_frame_to_dsp_image(params->dst, &dst_image, dst_planes, HAL_MAX_PLANES);
    r.src = &src_image;
    r.dst = &dst_image;
    r.interpolation = hal_interp_to_dsp(params->interpolation);

    dsp_roi_t crop{};
    crop.start_x = params->crop.start_x;
    crop.start_y = params->crop.start_y;
    crop.end_x   = params->crop.end_x;
    crop.end_y   = params->crop.end_y;

    dsp_letterbox_properties_t letterbox{};
    letterbox.alignment = hal_letterbox_to_dsp(params->letterbox_alignment);
    hal_color_to_dsp(&params->letterbox_color, &letterbox.color);

    if (params->scaling_mode == HAL_DSP_SCALING_STRETCH) {
        dsp_status st = dsp_crop_and_resize(ctx->device, &r, &crop);
        return dsp_status_to_hal(st);
    }

    dsp_status st = dsp_crop_and_resize_letterbox(ctx->device, &r, &crop, &letterbox);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_multi_crop_resize_sync(Hailo15DspContext *ctx, const HalDspMultiCropResizeParams *params)
{
    dsp_multi_crop_resize_params_t m{};
    dsp_image_properties_t src_image{};
    dsp_data_plane_t src_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->src, &src_image, src_planes, HAL_MAX_PLANES);
    m.src = &src_image;
    m.crop_resize_params_count = params->output_count;
    m.interpolation = hal_interp_to_dsp(params->interpolation);

    dsp_crop_resize_params_t crop_params_storage[DSP_MULTI_RESIZE_OUTPUTS_COUNT]{};
    m.crop_resize_params = crop_params_storage;

    dsp_image_properties_t dst_images[DSP_MULTI_RESIZE_OUTPUTS_COUNT]{};
    dsp_data_plane_t dst_planes[DSP_MULTI_RESIZE_OUTPUTS_COUNT][HAL_MAX_PLANES]{};
    dsp_roi_t crops[DSP_MULTI_RESIZE_OUTPUTS_COUNT]{};

    for (uint32_t i = 0; i < params->output_count && i < DSP_MULTI_RESIZE_OUTPUTS_COUNT; ++i) {
        const HalDspMultiCropOutput *out = &params->outputs[i];
        dsp_crop_resize_params_t *cp = &crop_params_storage[i];

        crops[i].start_x = out->crop.start_x;
        crops[i].start_y = out->crop.start_y;
        crops[i].end_x   = out->crop.end_x;
        crops[i].end_y   = out->crop.end_y;
        cp->crop = &crops[i];

        for (uint32_t j = 0; j < DSP_MULTI_RESIZE_OUTPUTS_COUNT; ++j) {
            cp->dst[j] = nullptr;
        }
        hal_frame_to_dsp_image(out->dst, &dst_images[i], dst_planes[i], HAL_MAX_PLANES);
        cp->dst[0] = &dst_images[i];

        dsp_scaling_properties_t scaling{};
        scaling.scaling_mode = hal_scaling_to_dsp(out->scaling_mode);
        hal_color_to_dsp(&out->letterbox_color, &scaling.color);
        cp->scaling_params[0] = scaling;
    }

    dsp_status st = dsp_multi_crop_and_resize(ctx->device, &m);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_blend_sync(Hailo15DspContext *ctx, const HalDspBlendParams *params)
{
    (void)ctx;
    dsp_image_properties_t base_image{};
    dsp_data_plane_t base_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->base, &base_image, base_planes, HAL_MAX_PLANES);

    static dsp_overlay_properties_t overlays_storage[50];
    dsp_data_plane_t overlays_planes[50][HAL_MAX_PLANES]{};
    size_t count = (params->overlay_count > 50) ? 50 : params->overlay_count;
    for (size_t i = 0; i < count; ++i) {
        HalDspOverlay *src_ov = &params->overlays[i];
        dsp_overlay_properties_t *dst_ov = &overlays_storage[i];
        hal_frame_to_dsp_image(src_ov->overlay, &dst_ov->overlay, overlays_planes[i], HAL_MAX_PLANES);
        dst_ov->x_offset = src_ov->x_offset;
        dst_ov->y_offset = src_ov->y_offset;
    }

    dsp_status st = dsp_blend(ctx->device, &base_image, overlays_storage, count);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_flip_rotate_sync(Hailo15DspContext *ctx, const HalDspFlipRotateParams *params)
{
    (void)ctx;
    dsp_affine_rotation_params_t r{};
    dsp_image_properties_t src_image{};
    dsp_image_properties_t dst_image{};
    dsp_data_plane_t src_planes[HAL_MAX_PLANES]{};
    dsp_data_plane_t dst_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->src, &src_image, src_planes, HAL_MAX_PLANES);
    hal_frame_to_dsp_image(params->dst, &dst_image, dst_planes, HAL_MAX_PLANES);
    r.src = &src_image;
    r.dst = &dst_image;
    r.interpolation = hal_interp_to_dsp(params->interpolation);

    switch (params->rotation_angle) {
    case HAL_DSP_ROTATION_ANGLE_90:  r.theta = 90.0f; break;
    case HAL_DSP_ROTATION_ANGLE_180: r.theta = 180.0f; break;
    case HAL_DSP_ROTATION_ANGLE_270: r.theta = 270.0f; break;
    case HAL_DSP_ROTATION_ANGLE_0:
    default:
        r.theta = 0.0f;
        break;
    }

    dsp_status st = dsp_rotate(ctx->device, &r);
    return dsp_status_to_hal(st);
}

static int hailo15_dsp_privacy_mask_sync(Hailo15DspContext *ctx, const HalDspPrivacyMaskParams *params)
{
    (void)ctx;
    if (!params || !params->image || !params->regions || params->region_count == 0) {
        return HAL_OK;
    }
    dsp_image_properties_t image{};
    dsp_data_plane_t img_planes[HAL_MAX_PLANES]{};
    hal_frame_to_dsp_image(params->image, &image, img_planes, HAL_MAX_PLANES);

    // Apply each region sequentially (in-place on the same image).
    for (uint32_t ri = 0; ri < params->region_count; ri++)
    {
        const HalDspPrivacyMaskRegion *region = &params->regions[ri];
        if (!region || !region->bitmask || region->stride_bytes == 0)
            continue;

        dsp_privacy_mask_t spm{};
        spm.bitmask = region->bitmask;
        spm.type = (region->type == HAL_DSP_PRIVACY_MASK_COLOR) ? DSP_PRIVACY_MASK_COLOR : DSP_PRIVACY_MASK_PIXELIZATION;
        if (spm.type == DSP_PRIVACY_MASK_COLOR) {
            hal_color_to_dsp(&region->color, &spm.color);
        } else {
            spm.blur_radius = region->blur_radius;
        }

        // ROI hints are in bitmask space (4x4 quantized image).
        std::array<dsp_roi_t, 8> rois_storage{};
        size_t rois_count = 0;
        if (region->rois && region->roi_count > 0)
        {
            const uint32_t want = (region->roi_count > 8U) ? 8U : region->roi_count;
            for (uint32_t i = 0; i < want; i++)
            {
                const HalDspRoi &r = region->rois[i];
                // Filter invalid ROIs (start must be < end in both axes).
                if (r.start_x >= r.end_x || r.start_y >= r.end_y)
                    continue;
                rois_storage[rois_count].start_x = (size_t)r.start_x;
                rois_storage[rois_count].start_y = (size_t)r.start_y;
                rois_storage[rois_count].end_x   = (size_t)r.end_x;
                rois_storage[rois_count].end_y   = (size_t)r.end_y;
                rois_count++;
                if (rois_count >= rois_storage.size())
                    break;
            }
        }
        spm.rois = (rois_count > 0) ? rois_storage.data() : nullptr;
        spm.rois_count = rois_count;

        unified_dsp_privacy_mask_t upm{};
        upm.type = spm.type;
        if (upm.type == DSP_PRIVACY_MASK_COLOR) {
            upm.color = spm.color;
        } else {
            upm.pixelization_size = spm.blur_radius;
        }
        upm.static_privacy_mask_params = &spm;
        upm.dynamic_privacy_mask_params = nullptr;

        dsp_status st = dsp_privacy_mask(ctx->device, &image, &upm);
        const int rc = dsp_status_to_hal(st);
        if (rc != HAL_OK)
            return rc;
    }
    return HAL_OK;
}

/* ---------------------- Ops table functions ---------------------- */

static int hailo15_dsp_init(const HalDspConfig *config, void **dsp_ctx_return)
{
    if (!dsp_ctx_return) {
        return HAL_ERR_INVALID_ARG;
    }

    Hailo15DspContext *ctx = new (std::nothrow) Hailo15DspContext();
    if (!ctx) {
        return HAL_ERR_NO_MEM;
    }

    ctx->device = nullptr;
    ctx->device_priority = config ? config->device_priority : 0;

    dsp_status st = dsp_create_device(&ctx->device);
    if (st != DSP_SUCCESS) {
        delete ctx;
        return dsp_status_to_hal(st);
    }

    if (ctx->device_priority != 0) {
        dsp_set_priority(ctx->device, ctx->device_priority);
    }

    ctx->stop_flag.store(false);
    ctx->worker = std::thread(hailo15_dsp_worker_thread, ctx);

    *dsp_ctx_return = ctx;
    return HAL_OK;
}

static int hailo15_dsp_deinit(void *dsp_ctx)
{
    if (!dsp_ctx) {
        return HAL_ERR_INVALID_ARG;
    }
    Hailo15DspContext *ctx = static_cast<Hailo15DspContext *>(dsp_ctx);

    {
        std::lock_guard<std::mutex> lock(ctx->queue_mtx);
        ctx->stop_flag.store(true);
    }
    ctx->queue_cv.notify_all();
    if (ctx->worker.joinable()) {
        ctx->worker.join();
    }

    if (ctx->device) {
        dsp_release_device(ctx->device);
        ctx->device = nullptr;
    }

    delete ctx;
    return HAL_OK;
}

static int hailo15_dsp_convert_format(void *dsp_ctx, const HalDspConvertFormatParams *params)
{
    if (!dsp_ctx || !params || !params->src || !params->dst) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_convert_format_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_resize(void *dsp_ctx, const HalDspResizeParams *params)
{
    if (!dsp_ctx || !params || !params->src || !params->dst) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_resize_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_crop_and_resize(void *dsp_ctx, const HalDspCropResizeParams *params)
{
    if (!dsp_ctx || !params || !params->src || !params->dst) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_crop_resize_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_multi_crop_and_resize(void *dsp_ctx, const HalDspMultiCropResizeParams *params)
{
    if (!dsp_ctx || !params || !params->src || !params->outputs || params->output_count == 0) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_multi_crop_resize_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_blend(void *dsp_ctx, const HalDspBlendParams *params)
{
    if (!dsp_ctx || !params || !params->base) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_blend_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_flip_rotate(void *dsp_ctx, const HalDspFlipRotateParams *params)
{
    if (!dsp_ctx || !params || !params->src || !params->dst) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_flip_rotate_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_privacy_mask(void *dsp_ctx, const HalDspPrivacyMaskParams *params)
{
    if (!dsp_ctx || !params || !params->image) {
        return HAL_ERR_INVALID_ARG;
    }
    return hailo15_dsp_privacy_mask_sync(static_cast<Hailo15DspContext *>(dsp_ctx), params);
}

static int hailo15_dsp_submit(void *dsp_ctx, HalDspOpType op_type, const void *params, HalDspJobHandle *job_out)
{
    if (!dsp_ctx || !params || !job_out) {
        return HAL_ERR_INVALID_ARG;
    }

    Hailo15DspContext *ctx = static_cast<Hailo15DspContext *>(dsp_ctx);
    HalDspJobHandle job = new (std::nothrow) HalDspJobTag;
    if (!job) {
        return HAL_ERR_NO_MEM;
    }
    job->op_type = op_type;
    job->result.status = HAL_DSP_JOB_PENDING;
    job->result.result_code = HAL_OK;
    job->completed.store(false);
    job->params_copy = nullptr;

    size_t param_size = 0;
    switch (op_type) {
    case HAL_DSP_OP_CONVERT_FORMAT:   param_size = sizeof(HalDspConvertFormatParams); break;
    case HAL_DSP_OP_RESIZE:          param_size = sizeof(HalDspResizeParams); break;
    case HAL_DSP_OP_CROP_RESIZE:     param_size = sizeof(HalDspCropResizeParams); break;
    case HAL_DSP_OP_MULTI_CROP_RESIZE: param_size = sizeof(HalDspMultiCropResizeParams); break;
    case HAL_DSP_OP_BLEND:           param_size = sizeof(HalDspBlendParams); break;
    case HAL_DSP_OP_FLIP_ROTATE:     param_size = sizeof(HalDspFlipRotateParams); break;
    case HAL_DSP_OP_PRIVACY_MASK:    param_size = sizeof(HalDspPrivacyMaskParams); break;
    default:
        delete job;
        return HAL_ERR_NOT_SUPPORTED;
    }

    void *copy = std::malloc(param_size);
    if (!copy) {
        delete job;
        return HAL_ERR_NO_MEM;
    }
    std::memcpy(copy, params, param_size);
    job->params_copy = copy;

    {
        std::lock_guard<std::mutex> lock(ctx->queue_mtx);
        ctx->job_queue.push(Hailo15DspJobItem{job});
    }
    ctx->queue_cv.notify_one();

    *job_out = job;
    return HAL_OK;
}

static int hailo15_dsp_wait(void *dsp_ctx, HalDspJobHandle job, uint32_t timeout_ms, HalDspJobResult *result_out)
{
    (void)dsp_ctx;
    if (!job) {
        return HAL_ERR_INVALID_ARG;
    }

    std::unique_lock<std::mutex> lock(job->mtx);
    if (!job->completed.load()) {
        if (timeout_ms == 0) {
            if (result_out) {
                *result_out = job->result;
            }
            return HAL_OK;
        }
        if (timeout_ms == UINT32_MAX) {
            job->cv.wait(lock, [&] { return job->completed.load(); });
        } else {
            if (!job->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [&] { return job->completed.load(); })) {
                return HAL_ERR_TIMEOUT;
            }
        }
    }

    if (result_out) {
        *result_out = job->result;
    }
    return HAL_OK;
}

static int hailo15_dsp_cancel(void *dsp_ctx, HalDspJobHandle job)
{
    (void)dsp_ctx;
    if (!job) {
        return HAL_ERR_INVALID_ARG;
    }
    if (job->completed.load()) {
        return HAL_ERR_INVALID_STATE;
    }
    {
        std::lock_guard<std::mutex> lock(job->mtx);
        job->result.status = HAL_DSP_JOB_CANCELLED;
        job->result.result_code = HAL_ERROR;
        job->completed.store(true);
    }
    job->cv.notify_all();
    return HAL_OK;
}

static int hailo15_dsp_job_release(void *dsp_ctx, HalDspJobHandle job)
{
    (void)dsp_ctx;
    if (!job) {
        return HAL_OK;
    }
    if (job->params_copy) {
        std::free(job->params_copy);
        job->params_copy = nullptr;
    }
    delete job;
    return HAL_OK;
}

static const char *hailo15_dsp_get_version(void)
{
    return "Hailo15 HAL-DSP 2.0.0";
}

HalDspOps HAL_DSP_OPS = {
    .init                 = hailo15_dsp_init,
    .deinit               = hailo15_dsp_deinit,
    .convert_format       = hailo15_dsp_convert_format,
    .resize               = hailo15_dsp_resize,
    .crop_and_resize      = hailo15_dsp_crop_and_resize,
    .multi_crop_and_resize = hailo15_dsp_multi_crop_and_resize,
    .blend                = hailo15_dsp_blend,
    .flip_rotate          = hailo15_dsp_flip_rotate,
    .privacy_mask         = hailo15_dsp_privacy_mask,
    .submit               = hailo15_dsp_submit,
    .wait                 = hailo15_dsp_wait,
    .cancel               = hailo15_dsp_cancel,
    .job_release          = hailo15_dsp_job_release,
    .get_version          = hailo15_dsp_get_version,
};

} /* extern "C" */

