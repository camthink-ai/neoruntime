/**
 * @file video_source.h
 * @brief Video Source - HAL Video wrapper with push-mode frame delivery
 *
 * Wraps HAL Video ops: init device, subscribe for callbacks.
 * Frames are delivered via FrameArrivalFn without any pixel copy.
 * Uses void* video_ctx; streams identified by name only.
 */

#pragma once

#include <string>
#include <vector>
#include <functional>
#include <unordered_map>
#include <mutex>

extern "C" {
    #include "hal_video.h"
}

/** Frame arrival callback — receives stream name and raw frame pointer */
using FrameArrivalFn = std::function<void(const std::string& stream_name,
                                           HalFrameBuffer* frame)>;

/** Per-stream slot within VideoSource */
struct StreamSlot {
    std::string     name;
    FrameArrivalFn  on_frame;
    bool            running = false;
};

class VideoSource {
public:
    explicit VideoSource(HalVideoOps* ops);
    ~VideoSource();

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    /**
     * @brief Initialize video device
     * @param config Video configuration
     * @return true on success
     */
    bool init(const HalVideoConfig& config);

    /**
     * @brief Initialize from pre-created FROM_MEDIA contexts (media pipeline mode)
     * @param video_list Array of HalVideoContext* pointers from get_video_list()
     * @param count Number of contexts
     * @return true on success
     *
     * Does NOT call HAL_VIDEO_OPS.init() — contexts are already created by
     * the media pipeline. Registers subscribe_stream callbacks for each context.
     */
    bool init_from_context(void** video_list, uint32_t count);
    void deinit();

    /** Start/stop individual stream by name */
    bool start_stream(const std::string& name);
    bool stop_stream(const std::string& name);
    void start_all();
    void stop_all();

    /**
     * @brief Register a stream by name
     * Must be called after init() and before set_frame_callback/start_stream.
     */
    void register_stream(const std::string& name);

    /**
     * @brief Register frame callback for a stream (called by FrameRouter)
     *
     * The callback receives raw HalFrameBuffer pointer. Caller must NOT
     * call release_frame; that is managed by FrameRouter ref counting.
     */
    void set_frame_callback(const std::string& stream_name, FrameArrivalFn fn);

    /**
     * @brief Release frame back to HAL buffer pool
     *
     * Only called by FrameRouter when ref_count reaches 0.
     */
    void release_frame(const std::string& stream_name, HalFrameBuffer* frame);

    void* video_ctx() const { return video_ctx_; }
    const std::vector<StreamSlot>& streams() const { return streams_; }

    /** Find stream slot by name, returns nullptr if not found */
    StreamSlot* find_stream(const std::string& name);
    const StreamSlot* find_stream(const std::string& name) const;

private:
    HalVideoOps*            ops_;
    void*                   video_ctx_ = nullptr;
    bool                    is_from_media_ = false;
    std::vector<StreamSlot> streams_;
    std::unordered_map<std::string, void*> stream_ctxs_;
    std::mutex              mu_;

    /** HAL callback (static trampoline -> instance method) */
    static void hal_frame_callback(void* video_ctx, HalFrameBuffer* frame,
                                   void* userdata);
};
