/**
 * @file osd_manager.cpp
 * @brief OSD Manager Implementation (v2: HalOsdOps + void* codec_ctx)
 */

#include "../include/osd_manager.h"
#include <cstring>

extern "C" {
    #include "hal_log.h"
}

OsdManager::OsdManager(HalOsdOps* ops) : ops_(ops) {}

int OsdManager::add_text(void* codec_ctx, const HalOsdTextOverlay& config) {
    if (!ops_ || !ops_->add_text_overlay) return -1;
    int ret = ops_->add_text_overlay(codec_ctx, &config);
    HAL_LOG_INFO("OsdManager: add_text [%s] on ctx=%p → %d",
                 config.base.id, codec_ctx, ret);
    return ret;
}

int OsdManager::set_text(void* codec_ctx, const HalOsdTextOverlay& config) {
    if (!ops_ || !ops_->set_text_overlay) return -1;
    int ret = ops_->set_text_overlay(codec_ctx, &config);
    HAL_LOG_INFO("OsdManager: set_text [%s] on ctx=%p → %d",
                 config.base.id, codec_ctx, ret);
    return ret;
}

int OsdManager::update_text(void* codec_ctx, const char* overlay_id, const char* text) {
    if (!ops_ || !ops_->set_text_overlay || !overlay_id || !text) return -1;
    // Build a full overlay struct with just the text changed
    HalOsdTextOverlay overlay{};
    strncpy(overlay.base.id, overlay_id, sizeof(overlay.base.id) - 1);
    overlay.base.type = HAL_OSD_OVERLAY_TEXT;
    overlay.base.enabled = true;
    strncpy(overlay.label, text, sizeof(overlay.label) - 1);
    return ops_->set_text_overlay(codec_ctx, &overlay);
}

int OsdManager::add_datetime(void* codec_ctx, const HalOsdDateTimeOverlay& config) {
    if (!ops_ || !ops_->add_datetime_overlay) return -1;
    int ret = ops_->add_datetime_overlay(codec_ctx, &config);
    HAL_LOG_INFO("OsdManager: add_datetime [%s] on ctx=%p → %d",
                 config.text.base.id, codec_ctx, ret);
    return ret;
}

int OsdManager::set_datetime(void* codec_ctx, const HalOsdDateTimeOverlay& config) {
    if (!ops_ || !ops_->set_datetime_overlay) return -1;
    int ret = ops_->set_datetime_overlay(codec_ctx, &config);
    HAL_LOG_INFO("OsdManager: set_datetime [%s] on ctx=%p → %d",
                 config.text.base.id, codec_ctx, ret);
    return ret;
}

int OsdManager::add_image(void* codec_ctx, const HalOsdImageOverlay& config) {
    if (!ops_ || !ops_->add_image_overlay) return -1;
    return ops_->add_image_overlay(codec_ctx, &config);
}

int OsdManager::set_image(void* codec_ctx, const HalOsdImageOverlay& config) {
    if (!ops_ || !ops_->set_image_overlay) return -1;
    int ret = ops_->set_image_overlay(codec_ctx, &config);
    HAL_LOG_INFO("OsdManager: set_image [%s] on ctx=%p → %d",
                 config.base.id, codec_ctx, ret);
    return ret;
}

int OsdManager::remove_overlay(void* codec_ctx, const char* overlay_id) {
    if (!ops_ || !ops_->remove_overlay) return -1;
    return ops_->remove_overlay(codec_ctx, overlay_id);
}

int OsdManager::clear_overlays(void* codec_ctx) {
    if (!ops_ || !ops_->clear_overlays) return -1;
    int ret = ops_->clear_overlays(codec_ctx);
    HAL_LOG_INFO("OsdManager: clear_overlays on ctx=%p → %d", codec_ctx, ret);
    return ret;
}

int OsdManager::set_overlay_enabled(void* codec_ctx, const char* overlay_id, bool enabled) {
    if (!ops_ || !ops_->set_overlay_enabled) return -1;
    return ops_->set_overlay_enabled(codec_ctx, overlay_id, enabled);
}
