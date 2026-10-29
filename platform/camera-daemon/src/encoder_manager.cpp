/**
 * @file encoder_manager.cpp
 * @brief Encoder Manager Implementation (v2: void* codec_ctx)
 */

#include "../include/encoder_manager.h"

extern "C" {
    #include "hal_log.h"
}

EncoderManager::EncoderManager(HalCodecOps* ops) : ops_(ops) {}

EncoderManager::~EncoderManager() {
    destroy_all();
}

bool EncoderManager::subscribe_output(void* ctx, const std::string& stream_name) {
    if (!ops_->subscribe) return true;
    int ret = ops_->subscribe(ctx, codec_output_callback, this);
    if (ret < 0) {
        HAL_LOG_ERROR("EncoderManager: subscribe failed for %s: %d",
                     stream_name.c_str(), ret);
        return false;
    }
    return true;
}

bool EncoderManager::create(const std::string& stream_name,
                            const HalCodecConfig& config) {
    if (!ops_) return false;

    bool needs_reclaim = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (encoders_.count(stream_name)) {
            HAL_LOG_WARNING("EncoderManager: Encoder already exists for %s",
                           stream_name.c_str());
            return true;
        }
        // destroy_all() may be mid-teardown: publishing a ctx right after
        // it reported all-clear would be state drift (the caller believes
        // the encoder set is empty and rebuilds the pipeline around it).
        if (destroy_all_active_) {
            HAL_LOG_ERROR("EncoderManager: create('%s') rejected — destroy_all "
                          "in progress", stream_name.c_str());
            return false;
        }
        // A previous destroy() of this name timed out and parked the old
        // ctx — reclaim it before registering a new one, or the next
        // destroy would silently overwrite (and permanently leak) it.
        needs_reclaim = closing_teardowns_.count(stream_name) > 0;
        if (!needs_reclaim && !creating_names_.insert(stream_name).second) {
            // Reserve the name across the unlocked HAL phase below: without
            // it two concurrent creates both pass this check and the later
            // insert overwrites (and leaks) the earlier ctx.
            HAL_LOG_WARNING("EncoderManager: create('%s') lost a concurrent "
                            "create race", stream_name.c_str());
            return false;
        }
    }
    if (needs_reclaim) {
        HAL_LOG_INFO("EncoderManager: create('%s') reclaiming parked teardown first",
                     stream_name.c_str());
        if (!destroy(stream_name)) {
            HAL_LOG_ERROR("EncoderManager: create('%s') rejected — teardown of "
                          "the previous encoder is still wedged in the HAL",
                          stream_name.c_str());
            return false;
        }
        {
            // Re-reserve after the reclaim window: a concurrent create may
            // have taken the name while mu_ was released around destroy().
            std::lock_guard<std::mutex> lock(mu_);
            if (destroy_all_active_ || encoders_.count(stream_name) ||
                !creating_names_.insert(stream_name).second) {
                HAL_LOG_WARNING("EncoderManager: create('%s') lost a concurrent "
                                "create race during reclaim", stream_name.c_str());
                return false;
            }
        }
    }

    // HAL creation runs without mu_ — the reservation owns the name.
    void* ctx = nullptr;
    int ret = ops_->init(&config, &ctx);
    if (ret < 0 || !ctx) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            creating_names_.erase(stream_name);
            creating_cv_.notify_all();
        }
        HAL_LOG_ERROR("EncoderManager: init failed for %s: %d",
                     stream_name.c_str(), ret);
        return false;
    }

    // Packets can arrive from the moment subscribe installs the callback;
    // until the promotion critical section below adds the routing maps,
    // the ctx is release-only so on_packet() drops-but-releases instead of
    // leaking the buffer reference.
    {
        std::lock_guard<std::mutex> lock(mu_);
        release_only_ctxs_.insert(ctx);
    }
    if (!subscribe_output(ctx, stream_name)) {
        ops_->deinit(ctx);
        {
            std::lock_guard<std::mutex> lock(mu_);
            release_only_ctxs_.erase(ctx);
            creating_names_.erase(stream_name);
            creating_cv_.notify_all();
        }
        return false;
    }

    EncoderSlot slot;
    slot.codec_ctx = ctx;
    slot.stream_name = stream_name;
    slot.running = false;
    slot.created_at = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mu_);
        creating_names_.erase(stream_name);
        creating_cv_.notify_all();
        encoders_[stream_name] = slot;
        handle_to_stream_[ctx] = stream_name;
        release_only_ctxs_.erase(ctx);  // promotion: routing maps live now
    }

    HAL_LOG_INFO("EncoderManager: Created encoder for %s ctx=%p",
                 stream_name.c_str(), ctx);
    return true;
}

bool EncoderManager::create_from_context(const std::string& stream_name, void* codec_ctx) {
    if (!ops_ || !codec_ctx) return false;

    bool needs_reclaim = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (encoders_.count(stream_name)) {
            HAL_LOG_WARNING("EncoderManager: Encoder already exists for %s",
                           stream_name.c_str());
            return true;
        }
        // See create(): reject during destroy_all() and never overwrite a
        // teardown parked by a drain abort; reserve the name across the
        // unlocked subscribe below.
        if (destroy_all_active_) {
            HAL_LOG_ERROR("EncoderManager: create_from_context('%s') rejected — "
                          "destroy_all in progress", stream_name.c_str());
            return false;
        }
        needs_reclaim = closing_teardowns_.count(stream_name) > 0;
        if (!needs_reclaim && !creating_names_.insert(stream_name).second) {
            HAL_LOG_WARNING("EncoderManager: create_from_context('%s') lost a "
                            "concurrent create race", stream_name.c_str());
            return false;
        }
    }
    if (needs_reclaim) {
        if (!destroy(stream_name)) {
            HAL_LOG_ERROR("EncoderManager: create_from_context('%s') rejected — "
                          "teardown of the previous encoder is still wedged in the HAL",
                          stream_name.c_str());
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (destroy_all_active_ || encoders_.count(stream_name) ||
                !creating_names_.insert(stream_name).second) {
                HAL_LOG_WARNING("EncoderManager: create_from_context('%s') lost "
                                "a concurrent create race during reclaim",
                                stream_name.c_str());
                return false;
            }
        }
    }

    // A RUNNING FROM_MEDIA encoder can deliver the moment subscribe()
    // installs the callback. Register the release-only guard FIRST so the
    // pre-promotion window (subscribe returned, routing maps not yet
    // written) drops-and-releases instead of leaking the buffer.
    {
        std::lock_guard<std::mutex> lock(mu_);
        release_only_ctxs_.insert(codec_ctx);
    }
    // HAL call without mu_ — the reservation owns the name.
    if (!subscribe_output(codec_ctx, stream_name)) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            release_only_ctxs_.erase(codec_ctx);
            creating_names_.erase(stream_name);
            creating_cv_.notify_all();
        }
        return false;
    }

    EncoderSlot slot;
    slot.codec_ctx = codec_ctx;
    slot.stream_name = stream_name;
    slot.running = true;   // FROM_MEDIA encoders are already running
    slot.from_media = true;
    slot.created_at = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mu_);
        creating_names_.erase(stream_name);
        creating_cv_.notify_all();
        encoders_[stream_name] = slot;
        handle_to_stream_[codec_ctx] = stream_name;
        release_only_ctxs_.erase(codec_ctx);  // promotion: routing live now
    }

    HAL_LOG_INFO("EncoderManager: Created FROM_MEDIA encoder for %s ctx=%p",
                 stream_name.c_str(), codec_ctx);
    return true;
}

int EncoderManager::encode_frame(const std::string& stream_name,
                                 HalFrameBuffer* frame) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end() || !it->second.running) return -1;
        if (!ops_->input_frame) return -1;
        ctx = it->second.codec_ctx;
    }
    int ret = ops_->input_frame(ctx, frame);
    if (ret != 0) {
        static int err_count = 0;
        if (++err_count <= 5) {
            HAL_LOG_ERROR("EncoderManager: input_frame(%s) failed: %d",
                         stream_name.c_str(), ret);
        }
    }
    return ret;
}

int EncoderManager::set_bitrate(const std::string& stream_name, uint32_t bitrate_bps) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    HalCodecConfig cfg{};
    cfg.bitrate = bitrate_bps;
    return ops_->dynamic_change_config(ctx, &cfg);
}

int EncoderManager::set_framerate(const std::string& stream_name, uint32_t fps) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    HalCodecConfig cfg{};
    cfg.framerate = fps;
    return ops_->dynamic_change_config(ctx, &cfg);
}

int EncoderManager::set_gop(const std::string& stream_name, uint32_t gop) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
    }
    if (!ops_->dynamic_change_config) return -1;
    // Use intra_pic_rate for the keyframe interval (I-frame distance).
    // gop_size in HalCodecConfig maps to Hantro gop_config.gop_size which
    // controls B-frame hierarchy (valid: 1-8), NOT the I-frame interval.
    // Setting gop_size=30 would break the encoder.
    HalCodecConfig cfg{};
    cfg.intra_pic_rate = gop;
    return ops_->dynamic_change_config(ctx, &cfg);
}

namespace {
// Bounded drain for in-flight force_keyframe() HAL calls before a context
// is torn down. medialib's force_idr has no deadline of its own; past this
// the teardown aborts rather than racing the call.
constexpr int kKeyframeDrainTimeoutMs = 2000;
/* Bounded wait for a same-name creation to finish its unlocked HAL phase
 * (encoder init/subscribe can take seconds). destroy()/destroy_all() wait
 * on this rather than reporting "nothing registered" while a ctx is about
 * to be published. */
constexpr int kCreateDrainTimeoutMs = 10000;
}  // namespace

int EncoderManager::force_keyframe(const std::string& stream_name) {
    // During media reinit the borrowed FROM_MEDIA ctx is freed while
    // encoders_ still maps the stale pointer; the gate keeps us out.
    if (is_rebuild_active()) return -1;
    if (!ops_ || !ops_->force_idr) return -1;
    // NEVER hold mu_ across the HAL call: force_idr can block indefinitely
    // inside medialib (gst_pad_send_event waiting on pad locks of a stalled
    // encode pipeline), and a mutex held across that block wedges every
    // has_encoder()/status caller daemon-wide (field deadlock, 10-29).
    // Lifetime is covered by the rebuild gate plus the in-flight drain in
    // wait_keyframe_calls_drained().
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        // Re-check under mu_: a rebuild that sets the gate just after the
        // fast-path check above must still exclude us — rebuilds drain
        // in-flight calls only after taking mu_, which orders against this
        // increment.
        if (rebuild_active_.load()) return -1;
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return -1;
        ctx = it->second.codec_ctx;
        ++keyframes_in_flight_;
    }
    const int rc = ops_->force_idr(ctx);
    {
        std::lock_guard<std::mutex> lock(mu_);
        --keyframes_in_flight_;
    }
    keyframe_drained_cv_.notify_all();
    return rc;
}

bool EncoderManager::wait_keyframe_calls_drained(int timeout_ms) {
    std::unique_lock<std::mutex> lock(mu_);
    if (keyframe_drained_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
            [this] { return keyframes_in_flight_ == 0; })) {
        return true;
    }
    HAL_LOG_ERROR("EncoderManager: force_idr still in flight after %dms "
                  "(stalled encode pipeline?); callers must abort the "
                  "teardown — recycling the ctx now would race the call",
                  timeout_ms);
    return false;
}

int EncoderManager::reconfigure(const std::string& stream_name,
                                const HalCodecConfig& new_config,
                                uint32_t* interrupt_ms) {
    auto start_time = std::chrono::steady_clock::now();

    bool was_running = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) {
            HAL_LOG_ERROR("EncoderManager: reconfigure - encoder not found: %s", stream_name.c_str());
            return -1;
        }
        was_running = it->second.running;
    }

    HAL_LOG_INFO("EncoderManager: Reconfiguring encoder %s (destroy+create, was_running=%d)",
                 stream_name.c_str(), was_running);

    if (!destroy(stream_name)) {
        HAL_LOG_ERROR("EncoderManager: reconfigure aborted for %s — previous "
                      "encoder teardown timed out on a wedged force_idr; "
                      "not creating a replacement against a parked ctx",
                      stream_name.c_str());
        return -1;
    }

    if (!create(stream_name, new_config)) {
        HAL_LOG_ERROR("EncoderManager: create failed during reconfigure for %s", stream_name.c_str());
        return -1;
    }

    if (was_running) {
        if (!start(stream_name)) {
            HAL_LOG_ERROR("EncoderManager: start failed during reconfigure for %s", stream_name.c_str());
            return -1;
        }
    }

    auto end_time = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    if (interrupt_ms) {
        *interrupt_ms = static_cast<uint32_t>(duration_ms.count());
    }

    HAL_LOG_INFO("EncoderManager: Reconfigured encoder %s via destroy+create (interrupt: %lums)",
                 stream_name.c_str(), interrupt_ms ? *interrupt_ms : 0);
    return 0;
}

void* EncoderManager::get_codec_ctx(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return nullptr;
    return it->second.codec_ctx;
}

void EncoderManager::codec_output_callback(void* codec_ctx,
                                            HalPacketBuffer* packet,
                                            void* userdata) {
    auto* self = static_cast<EncoderManager*>(userdata);
    self->on_packet(codec_ctx, packet);
}

void EncoderManager::on_packet(void* codec_ctx, const HalPacketBuffer* packet) {
    std::string stream_name;
    bool log_first = false;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto hit = handle_to_stream_.find(codec_ctx);
        if (hit == handle_to_stream_.end()) {
            // Release-only ctx — either a parked closing tombstone (destroy()
            // in its drain window, or a timed-out tombstone whose
            // subscription never got torn down) or a PENDING creation (the
            // running FROM_MEDIA encoder can deliver from the moment
            // subscribe() installs the callback, before the promotion
            // critical section adds the routing maps). The subscription is
            // live either way, so this packet owns a shared buffer
            // reference we MUST hand back — returning without release leaks
            // the pool one packet at a time (starvation → black screen).
            // ctxs dropped via discard_from_media() are NOT in the set and
            // keep the plain return: their ctx may already be freed, and
            // touching it would be the bigger bug.
            if (release_only_ctxs_.count(codec_ctx) && ops_ && ops_->release_packet) {
                ops_->release_packet(codec_ctx, const_cast<HalPacketBuffer*>(packet));
            }
            return;
        }
        stream_name = hit->second;

        // Stamp frame-health signal and emit the first-packet log once per slot
        // (NOT once per process — the old static-atomic suppressed first-packet
        // logs for every stream after the very first one).
        auto eit = encoders_.find(stream_name);
        if (eit != encoders_.end()) {
            auto& slot = eit->second;
            slot.last_packet_time = std::chrono::steady_clock::now();
            if (!slot.logged_first_packet) {
                slot.logged_first_packet = true;
                log_first = true;
            }
        }
    }

    if (log_first) {
        HAL_LOG_INFO("EncoderManager: First encoded packet for %s (size=%u)",
                    stream_name.c_str(), packet ? packet->size : 0);
    }

    if (output_fn_) {
        output_fn_(stream_name, packet);
    }

    if (ops_->release_packet) {
        ops_->release_packet(codec_ctx, const_cast<HalPacketBuffer*>(packet));
    }
}

// === Shared methods ===

bool EncoderManager::destroy(const std::string& stream_name) {
    void* ctx = nullptr;
    bool was_running = false;
    bool is_from_media = false;
    bool drained = true;
    {
        // Take the slot out of encoders_ FIRST, inside the same critical
        // section in-flight force_keyframe() calls increment under: once the
        // entry is gone no new call can capture the ctx, so one bounded wait
        // right here closes the capture/free race completely. On timeout the
        // slot stays parked in closing_teardowns_ — freeing a ctx a wedged
        // call still occupies is a use-after-free, so we leak it (loudly)
        // and let a later destroy() retry the reclamation. The parked entry
        // carries an exclusive claim (teardown_in_progress): a concurrent
        // destroy() of the same name must never run stop/unsubscribe/deinit
        // on a ctx another thread is already tearing down — for standalone
        // encoders that is a double-free.
        std::unique_lock<std::mutex> lock(mu_);
        // A creation of this name may be mid-HAL (reservation held, init/
        // subscribe running lock-free): returning "nothing registered"
        // would be a lie — the creator is about to publish a ctx the
        // caller believes is gone, re-introducing a dangling context into
        // a pipeline it is rebuilding or destroying. Wait bounded for the
        // reservation to clear, then re-look.
        if (creating_names_.count(stream_name)) {
            const bool creation_done = creating_cv_.wait_for(
                lock, std::chrono::milliseconds(kCreateDrainTimeoutMs),
                [&] { return creating_names_.find(stream_name) == creating_names_.end(); });
            if (!creation_done) {
                HAL_LOG_ERROR("EncoderManager: destroy('%s') aborted — creation "
                              "still inside the HAL after %dms",
                              stream_name.c_str(), kCreateDrainTimeoutMs);
                return false;
            }
        }
        auto it = encoders_.find(stream_name);
        if (it != encoders_.end()) {
            ctx = it->second.codec_ctx;
            was_running = it->second.running;
            is_from_media = it->second.from_media;

            handle_to_stream_.erase(ctx);
            EncoderSlot parked = it->second;
            parked.teardown_in_progress = true;  // this thread owns the teardown
            closing_teardowns_[stream_name] = parked;
            release_only_ctxs_.insert(ctx);
            encoders_.erase(it);
        } else {
            auto cit = closing_teardowns_.find(stream_name);
            if (cit == closing_teardowns_.end()) {
                return true;  // nothing registered under this name
            }
            if (cit->second.teardown_in_progress) {
                // Another thread owns this tombstone's teardown; report
                // not-torn-down instead of double-claiming the ctx.
                HAL_LOG_WARNING("EncoderManager: destroy('%s') — teardown "
                                "already owned by another call",
                                stream_name.c_str());
                return false;
            }
            // Retry of an aborted teardown: the parked ctx may be reclaimable
            // now if the wedged call has since returned.
            cit->second.teardown_in_progress = true;
            ctx = cit->second.codec_ctx;
            was_running = cit->second.running;
            is_from_media = cit->second.from_media;
        }
        drained = keyframe_drained_cv_.wait_for(
            lock, std::chrono::milliseconds(kKeyframeDrainTimeoutMs),
            [this] { return keyframes_in_flight_ == 0; });
    }

    if (!drained) {
        // Hand the claim back so a later retry can take over the tombstone.
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto cit = closing_teardowns_.find(stream_name);
            if (cit != closing_teardowns_.end()) {
                cit->second.teardown_in_progress = false;
            }
        }
        HAL_LOG_ERROR("EncoderManager: destroy('%s') aborted before HAL "
                      "teardown — force_idr still in flight after %dms; "
                      "ctx parked as closing tombstone (retry destroy to "
                      "reclaim) rather than racing the call",
                      stream_name.c_str(), kKeyframeDrainTimeoutMs);
        return false;
    }

    // FROM_MEDIA contexts are owned by media pipeline — skip start/stop/deinit
    int unsub_rc = 0;
    if (!is_from_media) {
        if (was_running && ops_->stop) {
            ops_->stop(ctx);
        }
        if (ops_->unsubscribe) {
            unsub_rc = ops_->unsubscribe(ctx, codec_output_callback);
        }
    } else if (ops_->unsubscribe) {
        unsub_rc = ops_->unsubscribe(ctx, codec_output_callback);
    }

    if (unsub_rc != 0) {
        // unsubscribe could not prove callback quiescence (the HAL reports
        // HAL_ERR_TIMEOUT when a copied callback outlives its bounded wait):
        // deinit would free the ctx under that live callback, and dropping
        // the release-only bookkeeping now would turn its late packets into
        // leaked buffer references. Park everything — same semantics as a
        // drain timeout: the tombstone keeps the ctx, late packets keep
        // getting released via release_only_ctxs_, and the claim is handed
        // back so a later destroy() retries (the subscription map entry is
        // already erased, so the retry only re-checks quiescence).
        {
            std::lock_guard<std::mutex> lock(mu_);
            auto cit = closing_teardowns_.find(stream_name);
            if (cit != closing_teardowns_.end()) {
                cit->second.teardown_in_progress = false;
            }
        }
        HAL_LOG_ERROR("EncoderManager: destroy('%s') aborted — unsubscribe "
                      "rc=%d (packet-callback quiesce not proven); ctx stays "
                      "parked as a closing tombstone (retry destroy to "
                      "reclaim)", stream_name.c_str(), unsub_rc);
        return false;
    }
    if (!is_from_media && ops_->deinit) {
        ops_->deinit(ctx);
    }

    // Drop the closing bookkeeping only AFTER unsubscribe: callbacks can
    // arrive until the subscription is actually gone, and while it lives
    // on_packet() needs the closing set to hand their buffers back.
    {
        std::lock_guard<std::mutex> lock(mu_);
        closing_teardowns_.erase(stream_name);
        release_only_ctxs_.erase(ctx);
    }

    HAL_LOG_INFO("EncoderManager: Destroyed encoder for %s (from_media=%d)",
                 stream_name.c_str(), is_from_media);
    return true;
}

bool EncoderManager::destroy_all() {
    // Exclusive ownership of the whole barrier window: with a plain flag,
    // the first finisher would clear destroy_all_active_ while a second
    // concurrent destroy_all() was still tearing down its snapshot — a
    // create slipping in then would miss the second call's window and
    // publish a ctx the second caller believes is gone. Serializing makes
    // each call's barrier cover its own complete snapshot+teardown.
    std::lock_guard<std::mutex> serialize(destroy_all_mu_);
    std::vector<std::string> names;
    {
        std::unique_lock<std::mutex> lock(mu_);
        // Barrier: reject new creations (create()/create_from_context()
        // check destroy_all_active_) for the whole snapshot+teardown window
        // and wait out in-flight ones — otherwise a create could publish a
        // fresh ctx right after this pass reported all-clear, and the
        // caller would tear the media pipeline down around a live encoder.
        destroy_all_active_ = true;
        const bool creations_drained = creating_cv_.wait_for(
            lock, std::chrono::milliseconds(kCreateDrainTimeoutMs),
            [this] { return creating_names_.empty(); });
        if (!creations_drained) {
            HAL_LOG_ERROR("EncoderManager: destroy_all() aborted — a creation "
                          "is still inside the HAL after %dms",
                          kCreateDrainTimeoutMs);
            destroy_all_active_ = false;
            return false;
        }
        std::unordered_set<std::string> seen;
        for (auto& [name, _] : encoders_) {
            if (seen.insert(name).second) names.push_back(name);
        }
        for (auto& [name, _] : closing_teardowns_) {
            // retry parked teardowns too (create() normally reclaims them,
            // but a name can linger if no re-registration ever happened)
            if (seen.insert(name).second) names.push_back(name);
        }
    }
    bool all_torn_down = true;
    for (auto& name : names) {
        if (!destroy(name)) {
            all_torn_down = false;
        }
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        destroy_all_active_ = false;
    }
    return all_torn_down;
}

void EncoderManager::discard_from_media() {
    std::lock_guard<std::mutex> lock(mu_);
    size_t discarded = 0;
    for (auto it = encoders_.begin(); it != encoders_.end(); ) {
        if (!it->second.from_media) {
            ++it;
            continue;
        }

        handle_to_stream_.erase(it->second.codec_ctx);
        it = encoders_.erase(it);
        ++discarded;
    }
    HAL_LOG_INFO("EncoderManager: Discarded %zu stale FROM_MEDIA context(s) without HAL access",
                 discarded);
}

bool EncoderManager::start(const std::string& stream_name) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return false;

    auto& slot = it->second;
    if (slot.running) return true;

    if (ops_->start) {
        int ret = ops_->start(slot.codec_ctx);
        if (ret < 0) {
            HAL_LOG_ERROR("EncoderManager: start failed for %s: %d",
                         stream_name.c_str(), ret);
            return false;
        }
    }

    slot.running = true;
    HAL_LOG_INFO("EncoderManager: Started encoder %s", stream_name.c_str());
    return true;
}

bool EncoderManager::stop(const std::string& stream_name) {
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = encoders_.find(stream_name);
        if (it == encoders_.end()) return false;

        auto& slot = it->second;
        if (!slot.running) return true;

        ctx = slot.codec_ctx;
        slot.running = false;
    }

    if (ops_->stop) {
        ops_->stop(ctx);
    }
    HAL_LOG_INFO("EncoderManager: Stopped encoder %s", stream_name.c_str());
    return true;
}

void EncoderManager::set_output_callback(PacketOutputFn fn) {
    output_fn_ = std::move(fn);
}

bool EncoderManager::has_encoder(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    return encoders_.count(stream_name) > 0;
}

uint64_t EncoderManager::ms_since_last_packet(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return UINT64_MAX;  // no encoder
    if (it->second.last_packet_time == std::chrono::steady_clock::time_point{}) {
        return UINT64_MAX;                          // never received a packet
    }
    auto delta = std::chrono::steady_clock::now() - it->second.last_packet_time;
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(delta).count());
}

bool EncoderManager::seen_first_packet(const std::string& stream_name) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return false;
    return it->second.last_packet_time != std::chrono::steady_clock::time_point{};
}

bool EncoderManager::is_stream_stalled(const std::string& stream_name,
                                       uint64_t threshold_ms,
                                       uint64_t grace_ms) const {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = encoders_.find(stream_name);
    if (it == encoders_.end()) return true;  // missing encoder counts as stalled

    const auto& slot = it->second;
    auto now = std::chrono::steady_clock::now();

    // Startup grace: a freshly-created encoder has not had a chance to emit yet.
    // Suppress stall verdicts during pipeline bring-up (incl. post-switch verify)
    // so we don't false-alarm a healthy but slow-to-first-frame encoder.
    auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - slot.created_at).count();
    if (static_cast<uint64_t>(age_ms) < grace_ms) return false;

    if (slot.last_packet_time == std::chrono::steady_clock::time_point{}) {
        return true;  // grace elapsed and still no first packet
    }
    auto gap_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - slot.last_packet_time).count();
    return static_cast<uint64_t>(gap_ms) > threshold_ms;
}
