/**
 * @file osd_manager.h
 * @brief OSD Manager - Overlay management via HalOsdOps
 *
 * Uses separate HalOsdOps with void* codec_ctx.
 */

#pragma once

#include <string>
#include <mutex>

extern "C" {
    #include "hal_osd.h"
}

class OsdManager {
public:
    explicit OsdManager(HalOsdOps* ops);
    ~OsdManager() = default;

    OsdManager(const OsdManager&) = delete;
    OsdManager& operator=(const OsdManager&) = delete;

    /** Add text overlay */
    int add_text(void* codec_ctx, const HalOsdTextOverlay& config);

    /** Update existing text overlay (matched by id) */
    int set_text(void* codec_ctx, const HalOsdTextOverlay& config);

    /** Update text content (sync) */
    int update_text(void* codec_ctx, const char* overlay_id, const char* text);

    /** Add datetime overlay */
    int add_datetime(void* codec_ctx, const HalOsdDateTimeOverlay& config);

    /** Update existing datetime overlay (matched by id) */
    int set_datetime(void* codec_ctx, const HalOsdDateTimeOverlay& config);

    /** Add image overlay */
    int add_image(void* codec_ctx, const HalOsdImageOverlay& config);

    /** Update existing image overlay (matched by id) */
    int set_image(void* codec_ctx, const HalOsdImageOverlay& config);

    /** Remove overlay */
    int remove_overlay(void* codec_ctx, const char* overlay_id);

    /** Remove all overlays from a stream */
    int clear_overlays(void* codec_ctx);

    /** Enable/disable overlay */
    int set_overlay_enabled(void* codec_ctx, const char* overlay_id, bool enabled);

private:
    HalOsdOps* ops_;
};
