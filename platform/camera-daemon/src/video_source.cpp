/**
 * @file video_source.cpp
 * @brief Video Source Implementation (v2: void* video_ctx)
 */

#include "../include/video_source.h"
#include <cstring>

extern "C" {
    #include "hal_log.h"
    #include "hal_video_internal.h"
}

VideoSource::VideoSource(HalVideoOps* ops) : ops_(ops) {}

VideoSource::~VideoSource() {
    deinit();
}

bool VideoSource::init(const HalVideoConfig& config) {
    if (!ops_ || !ops_->init) {
        HAL_LOG_ERROR("VideoSource: HAL video ops not available");
        return false;
    }

    void* ctx = nullptr;
    int ret = ops_->init(&config, &ctx);
    if (ret < 0 || !ctx) {
        HAL_LOG_ERROR("VideoSource: HAL init failed: %d", ret);
        return false;
    }
    video_ctx_ = ctx;
    HAL_LOG_INFO("VideoSource: Initialized video_ctx=%p", video_ctx_);

    return true;
}

bool VideoSource::init_from_context(void** video_list, uint32_t count) {
    if (!video_list || count == 0) {
        HAL_LOG_ERROR("VideoSource: empty video context list");
        return false;
    }

    // Clear previous state (reconfigure may call this again with new contexts)
    streams_.clear();
    stream_ctxs_.clear();

    // Use the first context as primary video_ctx
    video_ctx_ = video_list[0];
    is_from_media_ = true;
    HAL_LOG_INFO("VideoSource: init_from_context — %u video contexts, primary=%p",
                 count, video_ctx_);

    // Register streams from context names
    for (uint32_t i = 0; i < count; i++) {
        auto* vc = static_cast<HalVideoContext*>(video_list[i]);
        std::string name(vc->video_name);
        if (name.empty()) {
            HAL_LOG_WARNING("VideoSource: video context[%u] has empty name, skipping", i);
            continue;
        }

        // Store context for per-stream subscribe
        StreamSlot slot;
        slot.name = name;
        slot.running = false;
        streams_.push_back(std::move(slot));

        // Also store all contexts for per-stream callback dispatch
        stream_ctxs_[name] = video_list[i];

        HAL_LOG_INFO("VideoSource: Registered FROM_MEDIA stream [%s] ctx=%p",
                     name.c_str(), video_list[i]);
    }

    return true;
}

void VideoSource::deinit() {
    if (!video_ctx_) return;

    stop_all();
    streams_.clear();
    stream_ctxs_.clear();

    // FROM_MEDIA contexts are owned by media pipeline — do not deinit
    if (!is_from_media_ && ops_->deinit) {
        ops_->deinit(video_ctx_);
    }
    video_ctx_ = nullptr;
    is_from_media_ = false;
    HAL_LOG_INFO("VideoSource: Deinitialized");
}

void VideoSource::register_stream(const std::string& name) {
    StreamSlot slot;
    slot.name = name;
    slot.running = false;
    streams_.push_back(std::move(slot));
    HAL_LOG_INFO("VideoSource: Registered stream [%s]", name.c_str());
}

bool VideoSource::start_stream(const std::string& name) {
    StreamSlot* slot = find_stream(name);
    if (!slot) {
        HAL_LOG_ERROR("VideoSource: Stream not found: %s", name.c_str());
        return false;
    }
    if (slot->running) return true;

    // Pick the per-stream context for FROM_MEDIA, or primary ctx otherwise
    void* ctx = video_ctx_;
    auto it = stream_ctxs_.find(name);
    if (it != stream_ctxs_.end()) ctx = it->second;

    // Subscribe for push-mode frame delivery
    HAL_LOG_INFO("VideoSource: start_stream '%s' has_subscribe=%d has_callback=%d ctx=%p",
                 name.c_str(), (ops_->subscribe_stream ? 1 : 0),
                 (slot->on_frame ? 1 : 0), ctx);
    if (ops_->subscribe_stream && slot->on_frame) {
        int ret = ops_->subscribe_stream(ctx, name.c_str(),
                                          hal_frame_callback, slot);
        if (ret < 0) {
            HAL_LOG_ERROR("VideoSource: subscribe_stream(%s) failed: %d",
                         name.c_str(), ret);
            return false;
        }
        HAL_LOG_INFO("VideoSource: subscribe_stream('%s') OK", name.c_str());
    }

    // FROM_MEDIA: start/stop managed by media pipeline — skip
    if (!is_from_media_) {
        if (ops_->start) {
            int ret = ops_->start(video_ctx_);
            if (ret < 0) {
                HAL_LOG_ERROR("VideoSource: start() failed: %d", ret);
                if (ops_->unsubscribe_stream) {
                    ops_->unsubscribe_stream(video_ctx_, name.c_str());
                }
                return false;
            }
        }
    }

    slot->running = true;
    HAL_LOG_INFO("VideoSource: Started stream %s (from_media=%d)", name.c_str(), is_from_media_);
    return true;
}

bool VideoSource::stop_stream(const std::string& name) {
    StreamSlot* slot = find_stream(name);
    if (!slot || !slot->running) return true;

    void* ctx = video_ctx_;
    auto it = stream_ctxs_.find(name);
    if (it != stream_ctxs_.end()) ctx = it->second;

    // FROM_MEDIA: start/stop managed by media pipeline — skip
    if (!is_from_media_) {
        if (ops_->stop) {
            ops_->stop(video_ctx_);
        }
    }
    if (ops_->unsubscribe_stream) {
        ops_->unsubscribe_stream(ctx, name.c_str());
    }

    slot->running = false;
    HAL_LOG_INFO("VideoSource: Stopped stream %s", name.c_str());
    return true;
}

void VideoSource::start_all() {
    for (auto& slot : streams_) {
        start_stream(slot.name);
    }
}

void VideoSource::stop_all() {
    for (auto& slot : streams_) {
        stop_stream(slot.name);
    }
}

void VideoSource::set_frame_callback(const std::string& stream_name,
                                     FrameArrivalFn fn) {
    StreamSlot* slot = find_stream(stream_name);
    if (slot) {
        slot->on_frame = std::move(fn);
    }
}

void VideoSource::release_frame(const std::string& stream_name, HalFrameBuffer* frame) {
    if (!ops_->release_frame) return;
    // Use per-stream context if available (FROM_MEDIA)
    auto it = stream_ctxs_.find(stream_name);
    void* ctx = (it != stream_ctxs_.end()) ? it->second : video_ctx_;
    ops_->release_frame(ctx, frame);
}

StreamSlot* VideoSource::find_stream(const std::string& name) {
    for (auto& s : streams_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

const StreamSlot* VideoSource::find_stream(const std::string& name) const {
    for (auto& s : streams_) {
        if (s.name == name) return &s;
    }
    return nullptr;
}

/* Static trampoline: HAL callback → instance method */
void VideoSource::hal_frame_callback(void* video_ctx, HalFrameBuffer* frame,
                                     void* userdata) {
    auto* slot = static_cast<StreamSlot*>(userdata);
    if (slot && slot->on_frame) {
        slot->on_frame(slot->name, frame);
    }
    (void)video_ctx;
}

