/**
 * @file camera_daemon.cpp
 * @brief Camera Daemon - Top-level orchestrator implementation
 */

#include "../include/camera_daemon.h"
#include "../include/hal_loader.h"
#include "../include/video_source.h"
#include "../include/frame_router.h"
#include "../include/frame_watchdog.h"
#include "../include/osd_manager.h"
#include "../include/encoder_manager.h"
#include "../include/fd_publisher.h"
#include "../include/rtsp_server.h"
#include "../include/encoded_publisher.h"
#include "../include/ai_overlay_subscriber.h"
#include "../include/dpm_worker.h"
#include <dlfcn.h>

#ifdef HAS_GRPC
#include "../include/camera_control_service.h"
#include "../include/lens_hal_service.h"
#include "camera.pb.h"
#include <google/protobuf/util/json_util.h>
#endif

#include <thread>
#include <chrono>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

extern "C" {
    #include "hal_video.h"
    #include "hal_video_internal.h"
    #include "hal_codec.h"
    #include "hal_codec_internal.h"
    #include "hal_buffer.h"
    #include "hal_osd.h"
    #include "hal_media.h"
    #include "model/hal_draw.h"
    #include "hal_log.h"
}

namespace {

// DPM render modes (stored atomically as int for lock-free frontend reads;
// string form reconstructed for the API response by the helpers below).
constexpr int kDpmRenderMosaic = 0;
constexpr int kDpmRenderBlur = 1;
constexpr int kDpmRenderOverlay = 2;

// OSD config persistence mirror. /data/aipc/etc is the persistent p3 root; a
// .json suffix is NOT touched by deploy.sh (which only rewrites etc/*.yaml), so
// the mirror survives restart/deploy/OS-upgrade. Hardcoded (KISS) to match the
// C++ side's /data/aipc/etc default-root convention.
constexpr const char* kOsdConfigPath = "/data/aipc/etc/osd_config.json";

// Privacy-mask/DPM config persistence mirror. Same convention as the OSD mirror
// above: /data/aipc/etc is the persistent p3 root and a .json suffix is NOT
// clobbered by deploy.sh (which only rewrites etc/*.yaml), so the web-configured
// static privacy-mask regions + DPM labels/mode/color survive restart/deploy/
// OS-upgrade. The media library does not persist these (see set_privacy_mask_config),
// so without this mirror every restart drops back to the YAML defaults. Hardcoded
// (KISS) to match the C++ side's /data/aipc/etc default-root convention.
constexpr const char* kPrivacyMaskConfigPath = "/data/aipc/etc/privacy_mask.json";

// Transform (rotation/flip/dewarp/grayscale/dis/eis) config persistence mirror.
// Same convention as the OSD/privacy-mask mirrors: /data/aipc/etc is the
// persistent p3 root and a .json suffix is NOT clobbered by deploy.sh (which
// only rewrites etc/*.yaml), so web-configured dewarp/distortion etc. survive
// restart/deploy/OS-upgrade. The media library resets image_config to its YAML
// defaults on every pipeline (re)init, so without this mirror a restart drops
// runtime transform overrides. Hardcoded (KISS) to match the C++ side's
// /data/aipc/etc default-root convention.
constexpr const char* kTransformConfigPath = "/data/aipc/etc/transform_config.json";

// Scalar config-field persistence mirror. Same convention as the transform/OSD/
// privacy/ISP mirrors: /data/aipc/etc is the persistent p3 root and a .json
// suffix is NOT clobbered by deploy.sh (which only rewrites etc/*.yaml), so
// web-set scalar profile knobs survive camera-daemon restart/deploy/OS-upgrade.
// On boot this mirror is replayed via set_config_field so it overrides the HAL
// profile default (replay-on-boot wins over HAL's own profile persistence).
constexpr const char* kMediaConfigFieldsPath = "/data/aipc/etc/media_config_fields.json";

// Allow-list of writable scalar profile fields. config_field deliberately does
// NOT cover fields already owned by a typed RPC (dewarp/bitrate/gop/rotation/flip/
// ISP exposure) — exposing them here would create a two-writer race. Add a field
// only after confirming no typed RPC owns it.
static bool is_config_field_allowed(const std::string& path) {
    static const std::string kAllowed[] = {
        "frontend.hailort.use-hailort-service",  // VDevice sharing (see memory medialib-vdevice-sharing-fix)
    };
    for (const auto& a : kAllowed) {
        if (path == a) return true;
    }
    return false;
}

// ISP settings persistence mirror. Same convention as the OSD/privacy/transform
// mirrors: /data/aipc/etc is the persistent p3 root and a .json suffix is NOT
// clobbered by deploy.sh (which only rewrites etc/*.yaml), so web-tuned ISP
// values (brightness/contrast/saturation/sharpness, exposure, NR/WDR/powerline/
// AWB) survive restart/deploy/OS-upgrade. We persist a FULL-state snapshot
// (not the delta request) of cached_isp_state_ after every successful apply so
// a partial update replays the complete ISP state. Hardcoded (KISS) to match
// the C++ side's /data/aipc/etc default-root convention.
constexpr const char* kIspConfigPath = "/data/aipc/etc/isp_config.json";

// Active-profile persistence mirror. Same convention as the OSD/privacy/transform/
// ISP mirrors: /data/aipc/etc is the persistent p3 root and a .json suffix is NOT
// clobbered by deploy.sh (which only rewrites etc/*.yaml), so a web profile change
// survives restart/deploy/OS-upgrade. Content is plain-string JSON
// {"profile_name":"..."} (no proto) so the helpers are unconditional. Hardcoded
// (KISS) to match the C++ side's /data/aipc/etc default-root convention.
constexpr const char* kProfileConfigPath = "/data/aipc/etc/profile_config.json";

int dpm_render_mode_from_string(const std::string& s) {
    if (s == "blur") return kDpmRenderBlur;
    if (s == "overlay") return kDpmRenderOverlay;
    return kDpmRenderMosaic;  // default + "mosaic"
}

const char* dpm_render_mode_to_string(int mode) {
    switch (mode) {
        case kDpmRenderBlur:    return "blur";
        case kDpmRenderOverlay: return "overlay";
        default:                return "mosaic";
    }
}

void apply_flip_to_normalized_point(HalFlipDirection flip, float* x, float* y) {
    if (!x || !y) return;
    if (flip == HAL_FLIP_DIRECTION_HORIZONTAL || flip == HAL_FLIP_DIRECTION_BOTH) {
        *x = 1.0f - *x;
    }
    if (flip == HAL_FLIP_DIRECTION_VERTICAL || flip == HAL_FLIP_DIRECTION_BOTH) {
        *y = 1.0f - *y;
    }
}

// Privacy-mask API coordinates are display-space coordinates. When only the
// flip changes, migrate the cached points from the old displayed frame into
// the new displayed frame: undo the old flip, then apply the new flip.
void remap_privacy_mask_flip(std::vector<HalPrivacyMaskItem>* items,
                             HalFlipDirection old_flip,
                             HalFlipDirection new_flip) {
    if (!items || old_flip == new_flip) return;
    for (auto& item : *items) {
        for (int i = 0; i < 8 && item.points[i].x >= 0.0f; ++i) {
            apply_flip_to_normalized_point(old_flip, &item.points[i].x, &item.points[i].y);
            apply_flip_to_normalized_point(new_flip, &item.points[i].x, &item.points[i].y);
            item.points[i].x = std::clamp(item.points[i].x, 0.0f, 1.0f);
            item.points[i].y = std::clamp(item.points[i].y, 0.0f, 1.0f);
        }
    }
}

#ifdef HAS_GRPC
float clamp_osd_unit(float value, bool* changed) {
    if (!std::isfinite(value)) {
        if (changed) *changed = true;
        return 0.0f;
    }
    if (value < 0.0f) {
        if (changed) *changed = true;
        return 0.0f;
    }
    if (value > 1.0f) {
        if (changed) *changed = true;
        return 1.0f;
    }
    return value;
}

bool valid_osd_positive_unit(float value) {
    return std::isfinite(value) && value > 0.0f && value <= 1.0f;
}

bool readable_file(const std::string& path) {
    if (path.empty()) return false;
    struct stat st {};
    return ::stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && ::access(path.c_str(), R_OK) == 0;
}

bool sanitize_osd_config_request(const aipc::camera::OsdConfigRequest& in,
                                 aipc::camera::OsdConfigRequest* out) {
    if (!out) return false;
    bool changed = false;
    out->Clear();
    out->set_suppress_bake(in.suppress_bake());

    for (const auto& stream : in.streams()) {
        if (stream.stream_name().empty()) {
            HAL_LOG_WARNING("CameraDaemon: dropping OSD stream with empty stream_name");
            changed = true;
            continue;
        }

        auto* dst_stream = out->add_streams();
        dst_stream->set_stream_name(stream.stream_name());

        for (const auto& text : stream.text_overlays()) {
            if (text.id().empty()) {
                HAL_LOG_WARNING("CameraDaemon: dropping text OSD with empty id on stream '%s'",
                                stream.stream_name().c_str());
                changed = true;
                continue;
            }
            auto* dst = dst_stream->add_text_overlays();
            dst->CopyFrom(text);
            dst->set_x(clamp_osd_unit(text.x(), &changed));
            dst->set_y(clamp_osd_unit(text.y(), &changed));
            if (!std::isfinite(text.font_size()) || text.font_size() <= 0.0f) {
                dst->set_font_size(24.0f);
                changed = true;
            }
        }

        for (const auto& dt : stream.datetime_overlays()) {
            if (dt.id().empty()) {
                HAL_LOG_WARNING("CameraDaemon: dropping datetime OSD with empty id on stream '%s'",
                                stream.stream_name().c_str());
                changed = true;
                continue;
            }
            auto* dst = dst_stream->add_datetime_overlays();
            dst->CopyFrom(dt);
            dst->set_x(clamp_osd_unit(dt.x(), &changed));
            dst->set_y(clamp_osd_unit(dt.y(), &changed));
            if (!std::isfinite(dt.font_size()) || dt.font_size() <= 0.0f) {
                dst->set_font_size(24.0f);
                changed = true;
            }
        }

        for (const auto& img : stream.image_overlays()) {
            if (img.id().empty()) {
                HAL_LOG_WARNING("CameraDaemon: dropping image OSD with empty id on stream '%s'",
                                stream.stream_name().c_str());
                changed = true;
                continue;
            }
            const std::string image_path = img.image_path();
            if (!readable_file(image_path)) {
                HAL_LOG_WARNING("CameraDaemon: dropping image OSD '%s' on stream '%s': unreadable image_path='%s'",
                                img.id().c_str(), stream.stream_name().c_str(), image_path.c_str());
                changed = true;
                continue;
            }
            if (!valid_osd_positive_unit(img.width()) || !valid_osd_positive_unit(img.height())) {
                HAL_LOG_WARNING("CameraDaemon: dropping image OSD '%s' on stream '%s': invalid size %.3fx%.3f",
                                img.id().c_str(), stream.stream_name().c_str(), img.width(), img.height());
                changed = true;
                continue;
            }

            auto* dst = dst_stream->add_image_overlays();
            dst->CopyFrom(img);
            dst->set_x(clamp_osd_unit(img.x(), &changed));
            dst->set_y(clamp_osd_unit(img.y(), &changed));
        }
    }

    if (changed) {
        HAL_LOG_WARNING("CameraDaemon: sanitized unsafe OSD config before applying");
    }
    return changed;
}
#endif

bool runtime_stream_reconfiguration_enabled() {
    const char* value = std::getenv("AIPC_ALLOW_RUNTIME_STREAM_RECONFIG");
    if (value == nullptr) return true;  // default: enabled
    return !(std::strcmp(value, "0") == 0 ||
             std::strcmp(value, "false") == 0 ||
             std::strcmp(value, "no") == 0);
}

}  // namespace

CameraDaemon::CameraDaemon() = default;

CameraDaemon::~CameraDaemon() {
    shutdown();
}

bool CameraDaemon::init(const DaemonConfig& config) {
    config_ = config;

    HAL_LOG_INFO("CameraDaemon: Initializing...");

    // Strict init order: HAL → Media → Video → Encoders (+ OSD) → RTSP → EncodedPub → Watchdog
    if (!init_hal()) return false;
    init_mcu_context();
    if (!init_media()) return false;
    if (!init_video()) return false;
    if (!init_encoders()) return false;
#ifdef HAS_GRPC
    // Reapply persisted OSD overlays now that osd_mgr_/encoder_mgr_/codec ctx
    // are all live. Reuses the single update_osd_config apply path (which also
    // re-mirrors to disk — same content, idempotent, one cheap startup write).
    {
        aipc::camera::OsdConfigRequest persisted;
        if (load_osd_config(&persisted)) {
            HAL_LOG_INFO("CameraDaemon: applying persisted OSD config (%d stream(s))",
                         persisted.streams_size());
            update_osd_config(persisted);
        }
    }
    // Reapply persisted privacy-mask/DPM config. init_media() ran above, so
    // media_ctx_ is live and set_privacy_mask_config can apply immediately. On
    // miss/corrupt/empty load_* returns false and we start from YAML defaults.
    // Re-applying re-persists (idempotent, one cheap startup write) — same shape
    // as the OSD replay above.
    {
        aipc::camera::PrivacyMaskConfig persisted;
        if (load_privacy_mask_config(&persisted)) {
            HAL_LOG_INFO("CameraDaemon: applying persisted privacy-mask config (enabled=%d regions=%d dpm=%d)",
                         persisted.enabled() ? 1 : 0, persisted.regions_size(),
                         persisted.dpm_enabled() ? 1 : 0);
            set_privacy_mask_config(persisted);
        }
    }
    // Reapply persisted transform (rotation/flip/dewarp/grayscale/dis/eis).
    // init_media() ran above so media_ctx_ is live and set_transform_config can
    // apply immediately. On miss/corrupt/identity load_transform_config returns
    // false and we start from the YAML defaults. Re-applying re-persists
    // (idempotent, one cheap startup write) — same shape as the OSD/privacy
    // replays above. NOTE: dewarp replay only re-enables the dewarp image field;
    // the MEDIALIB_DEWARP_DSP_OPTIMIZATION env (kept at 0, the sp805-watchdog-
    // safe setting) is a separate process env, not touched here.
    {
        aipc::camera::TransformConfig persisted;
        if (load_transform_config(&persisted)) {
            HAL_LOG_INFO("CameraDaemon: applying persisted transform config (rot=%d flip=%d dewarp=%d gray=%d dis=%d eis=%d)",
                         (int)persisted.rotation(), (int)persisted.flip(),
                         persisted.dewarp() ? 1 : 0, persisted.grayscale() ? 1 : 0,
                         persisted.dis() ? 1 : 0, persisted.eis() ? 1 : 0);
            set_transform_config(persisted);
        }
    }
    // Reapply persisted scalar config fields (replay-on-boot: platform mirror
    // wins over HAL profile defaults, resolving the two-writer ambiguity — HAL's
    // own profile persistence becomes an idempotent fallback). Loaded AFTER
    // init_media so media_ctx_ is live. Calls HAL_MEDIA_OPS.set_config_field
    // directly (not the set_config_field RPC method) so we don't re-persist the
    // mirror we just read, and the allow-list check is defense-in-depth against
    // a stale mirror carrying a field that later left the allow-list.
    // Best-effort: a per-field HAL failure logs WARN but never aborts init.
    {
        aipc::camera::MediaConfigFields persisted;
        if (load_config_fields(&persisted)) {
            auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
            for (const auto& kv : persisted.fields()) {
                const std::string& path = kv.first;
                const auto& fv = kv.second;
                if (!is_config_field_allowed(path)) {
                    HAL_LOG_WARNING("CameraDaemon: config-field replay: skipping non-allow-listed %s",
                                    path.c_str());
                    continue;
                }
                if (!media_ops || !media_ctx_ || !media_ops->set_config_field) {
                    HAL_LOG_WARNING("CameraDaemon: config-field replay: media not ready, deferring %s",
                                    path.c_str());
                    break;
                }
                const HalConfigFieldType hal_type = static_cast<HalConfigFieldType>(fv.type());
                int rc = media_ops->set_config_field(media_ctx_, path.c_str(),
                                                     hal_type, fv.value().c_str());
                if (rc < 0) {
                    HAL_LOG_WARNING("CameraDaemon: config-field replay: set %s=%s failed (rc=%d)",
                                    path.c_str(), fv.value().c_str(), rc);
                } else {
                    HAL_LOG_INFO("CameraDaemon: config-field replay: set %s=%s",
                                 path.c_str(), fv.value().c_str());
                }
            }
        }
    }
    // Reapply persisted ISP settings (full-state snapshot of cached_isp_state_).
    // init_video() ran above so video_source_->video_ctx() is live and
    // update_isp_settings can apply immediately. On miss/corrupt load_isp_config
    // returns false and we start from the HAL/cached defaults. The persisted
    // request has every field set (always_print_primitive_fields), so the replay
    // fires all three branches of update_isp_settings and re-pushes the complete
    // ISP state (manual tuning + exposure + NR/WDR/powerline/AWB). Re-applying
    // re-persists (idempotent, one cheap startup write) — same shape as the
    // OSD/privacy/transform replays above.
    {
        aipc::camera::ISPUpdateRequest persisted;
        if (load_isp_config(&persisted)) {
            HAL_LOG_INFO("CameraDaemon: applying persisted ISP config (B=%d C=%d S=%d Sh=%d AE=%d NR=%d WDR=%d PF=%d AWB=%d)",
                         persisted.brightness(), persisted.contrast(),
                         persisted.saturation(), persisted.sharpness(),
                         persisted.auto_exposure() ? 1 : 0,
                         persisted.noise_reduction(), persisted.wdr_value(),
                         persisted.powerline_freq(), persisted.awb_index());
            update_isp_settings(persisted);
        }
    }
#endif
    if (!init_rtsp()) return false;
    if (!init_encoded_publisher()) return false;
    if (!init_ai_overlay()) return false;

    // Initialize audio service if HAL supports it and config enables it
    if (hal_loader_->has_audio() && config_.audio.enabled) {
        audio_service_ = std::make_unique<AudioService>(hal_loader_->audio());
        if (encoded_pub_) {
            audio_service_->set_encoded_publisher(encoded_pub_.get());
            EncodedPublisher::StreamConfig audio_sc;
            audio_sc.name = "audio_capture";
            audio_sc.codec = config_.audio.codec;
            audio_sc.width = 0;
            audio_sc.height = 0;
            encoded_pub_->add_stream(audio_sc, config_.encoded_pub_dir);
        }
        if (!audio_service_->init(config_.audio)) {
            HAL_LOG_WARNING("CameraDaemon: Audio service init failed, audio disabled");
            audio_service_.reset();
        } else {
            audio_service_->start_capture();
            HAL_LOG_INFO("CameraDaemon: Audio capture auto-started");
        }
    }

    // Start watchdog
    WatchdogConfig wdcfg;
    wdcfg.scan_interval = std::chrono::milliseconds(config_.watchdog_scan_ms);
    wdcfg.frame_timeout = std::chrono::milliseconds(config_.watchdog_timeout_ms);
    wdcfg.warn_threshold = std::chrono::milliseconds(config_.watchdog_warn_ms);

    watchdog_ = std::make_unique<FrameWatchdog>(wdcfg);
    frame_router_ = std::make_unique<FrameRouter>(video_source_.get(),
                                                   watchdog_.get());

    // FD publisher (for trusted Apps with dma_buf permission)
    FdPublisherConfig fd_cfg;
    fd_cfg.sock_path = config_.fd_pub_sock_path;
    fd_cfg.max_clients = config_.fd_pub_max_clients;
    fd_cfg.max_outstanding_per_client = config_.fd_pub_max_outstanding;
    fd_pub_ = std::make_unique<FdPublisher>(frame_router_.get(), fd_cfg);

    // Register all subscribers with FrameRouter
    register_subscribers();

    // Start async dispatch thread (must be after subscriber registration)
    frame_router_->start();

    // Wire VideoSource callbacks to FrameRouter through the pre-encode overlay path.
    bind_video_source_callbacks();

    // Start watchdog with reclaim callback
    watchdog_->start([this](uint64_t frame_id) {
        frame_router_->force_reclaim(frame_id);
    });

    // Start FD publisher UDS server
    if (fd_pub_ && !config_.fd_pub_sock_path.empty()) {
        if (!fd_pub_->start()) {
            HAL_LOG_WARNING("CameraDaemon: FD publisher failed to start, "
                          "zero-copy FD delivery unavailable");
        }
    }

    // Reapply persisted active profile. Placed LATE in init — after init_rtsp()
    // and the encoded/FD publishers are up (switch_profile tears down + rebuilds
    // those consumers), and before start_grpc_server so no RPC can race the
    // replay. GUARDED: read the HAL's current profile and only switch when the
    // persisted name differs, so a clean boot whose YAML/default profile already
    // matches does NOT rebuild the pipeline (avoids startup churn). A failed
    // replay is logged + swallowed: the running default profile is still valid.
    {
        std::string persisted_profile;
        if (load_profile_config(&persisted_profile)) {
            std::string current = get_current_profile();
            if (!persisted_profile.empty() && persisted_profile != current) {
                HAL_LOG_INFO("CameraDaemon: applying persisted profile '%s' (current '%s')",
                             persisted_profile.c_str(), current.c_str());
                std::string msg;
                if (!switch_profile(persisted_profile, &msg)) {
                    HAL_LOG_WARNING("CameraDaemon: replay profile switch to '%s' failed: %s; continuing with '%s'",
                                    persisted_profile.c_str(), msg.c_str(), current.c_str());
                }
            } else {
                HAL_LOG_INFO("CameraDaemon: persisted profile '%s' already active; no replay switch",
                             persisted_profile.c_str());
            }
        }
    }

#ifdef HAS_GRPC
    start_grpc_server();
#endif

    HAL_LOG_INFO("CameraDaemon: Initialization complete");
    return true;
}

void CameraDaemon::run() {
    // Set running first, then honor the latched stop request. This ordering
    // closes both races:
    //   1) SIGTERM during init() is remembered by stop_requested_.
    //   2) SIGTERM after this store clears running_ in stop().
    running_.store(true);
    if (stop_requested_.load()) {
        running_.store(false);
        HAL_LOG_INFO("CameraDaemon: Stop requested during initialization; skipping pipeline start");
        return;
    }

    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (media_ops && media_ctx_) {
        // v2 media pipeline mode: unified start
        // IMPORTANT: subscribe_stream must be called BEFORE start_pipeline(),
        // because start_pipeline() blocks and never returns — frame delivery
        // happens via GStreamer callbacks that look up video_subscribers[].
        for (auto& slot : video_source_->streams()) {
            HAL_LOG_INFO("CameraDaemon: Subscribing stream '%s' before pipeline start",
                         slot.name.c_str());
            video_source_->start_stream(slot.name);
        }

        HAL_LOG_INFO("CameraDaemon: Starting media pipeline");
        int ret = media_ops->start(media_ctx_);
        if (ret < 0) {
            HAL_LOG_ERROR("CameraDaemon: media_ops->start() failed: %d", ret);
            running_.store(false);
            return;
        }

        // Prime the image-config cache from the freshly-built pipeline. Runtime
        // transform overrides (rotation/flip/dewarp/grayscale) live only in the
        // media context and must be restored after any later deinit+init rebuild
        // (e.g. stream re-enable); capture the file-derived defaults as a baseline.
        if (media_ops->get_current_config) {
            HalMediaConfig cur;
            memset(&cur, 0, sizeof(cur));
            if (media_ops->get_current_config(media_ctx_, &cur) >= 0) {
                last_image_config_ = cur.image_config;
                have_last_image_config_ = true;
            }
        }
    } else {
        // Legacy mode: start video and encoders separately
        video_source_->start_all();
        for (auto& ec : config_.encoders) {
            if (!ec.enabled) {
                HAL_LOG_INFO("CameraDaemon: Skipping disabled encoder '%s'", ec.stream_name.c_str());
                continue;
            }
            encoder_mgr_->start(ec.stream_name);
        }
    }

    HAL_LOG_INFO("CameraDaemon: Running, streams active");

    // Main loop: just wait for stop signal
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    HAL_LOG_INFO("CameraDaemon: Stop signal received");
}

void CameraDaemon::stop() {
    stop_requested_.store(true);
    running_.store(false);
}

void CameraDaemon::init_isp_cache() {
    auto* isp_ops = hal_loader_ ? hal_loader_->isp() : nullptr;
    void* ctx = video_source_ ? video_source_->video_ctx() : nullptr;

    if (isp_ops && ctx && isp_ops->get_current_image_config) {
        if (isp_ops->get_current_image_config(ctx, &cached_isp_state_) >= 0) {
            isp_state_initialized_ = true;
            HAL_LOG_INFO("CameraDaemon: ISP cache initialized from HAL");
            return;
        }
    }

    // Fallback: use sensible defaults (matches stub HAL defaults)
    memset(&cached_isp_state_, 0, sizeof(cached_isp_state_));
    cached_isp_state_.pwr_freq = HAL_ISP_PWR_FREQ_50HZ;
    cached_isp_state_.noise_reduction = 50;
    cached_isp_state_.wdr_value = 0;
    cached_isp_state_.awb_idx = 0;
    cached_isp_state_.manual_config.manual_state = false;
    cached_isp_state_.manual_config.brightness = 50;
    cached_isp_state_.manual_config.contrast = 50;
    cached_isp_state_.manual_config.saturation = 50;
    cached_isp_state_.manual_config.sharpness = 50;
    cached_isp_state_.exposure_config.auto_exposure = true;
    cached_isp_state_.exposure_config.backlight = 50;
    cached_isp_state_.exposure_config.exposure_time_us = 0;
    cached_isp_state_.exposure_config.gain = 0;
    isp_state_initialized_ = true;
    HAL_LOG_INFO("CameraDaemon: ISP cache initialized with defaults");
}

bool CameraDaemon::update_isp_settings(const aipc::camera::ISPUpdateRequest& request) {
    auto* isp_ops = hal_loader_ ? hal_loader_->isp() : nullptr;
    if (!isp_ops) {
        HAL_LOG_WARNING("CameraDaemon: ISP ops not available");
        return false;
    }

    void* ctx = video_source_ ? video_source_->video_ctx() : nullptr;
    if (!ctx) {
        HAL_LOG_WARNING("CameraDaemon: No video context for ISP update");
        return false;
    }

    // Initialize cache on first call (may have missed in init if HAL wasn't ready)
    if (!isp_state_initialized_) init_isp_cache();

    // Apply manual tuning — use has_xxx() to detect explicitly-set fields
    // This correctly handles value=0 (e.g., brightness=0) as a valid setting
    if (request.has_manual_mode() || request.has_brightness() || request.has_contrast()
        || request.has_saturation() || request.has_sharpness()) {
        HalIspManualConfig mc = cached_isp_state_.manual_config;
        if (request.has_manual_mode())  mc.manual_state = request.manual_mode();
        if (request.has_brightness())    mc.brightness   = request.brightness();
        if (request.has_contrast())      mc.contrast      = request.contrast();
        if (request.has_saturation())    mc.saturation    = request.saturation();
        if (request.has_sharpness())     mc.sharpness     = request.sharpness();
        int ret = isp_ops->set_manual_config(ctx, &mc);
        if (ret < 0) {
            HAL_LOG_ERROR("CameraDaemon: set_manual_config failed: %d", ret);
            return false;
        }
        cached_isp_state_.manual_config = mc;
        HAL_LOG_INFO("CameraDaemon: Manual config applied: mode=%d B=%d C=%d S=%d Sh=%d",
                     mc.manual_state, mc.brightness, mc.contrast, mc.saturation, mc.sharpness);
    }

    // Apply exposure config
    if (request.has_auto_exposure() || request.has_backlight()
        || request.has_exposure_time_us() || request.has_gain()) {
        HalIspExposureConfig ec = cached_isp_state_.exposure_config;
        if (request.has_auto_exposure())    ec.auto_exposure   = request.auto_exposure();
        if (request.has_backlight())        ec.backlight       = request.backlight();
        if (request.has_exposure_time_us()) ec.exposure_time_us = request.exposure_time_us();
        if (request.has_gain())             ec.gain            = request.gain();
        int ret = isp_ops->set_exposure_config(ctx, &ec);
        if (ret < 0) {
            HAL_LOG_ERROR("CameraDaemon: set_exposure_config failed: %d", ret);
            return false;
        }
        cached_isp_state_.exposure_config = ec;
        HAL_LOG_INFO("CameraDaemon: Exposure config applied: AE=%d BL=%d ET=%d G=%d",
                     ec.auto_exposure, ec.backlight, ec.exposure_time_us, ec.gain);
    }

    // Apply full image config (noise reduction, WDR, powerline freq, AWB)
    if (request.has_noise_reduction() || request.has_wdr_value()
        || request.has_powerline_freq() || request.has_awb_index()) {
        HalIspImageConfig ic = cached_isp_state_;
        if (request.has_noise_reduction()) ic.noise_reduction = request.noise_reduction();
        if (request.has_wdr_value())       ic.wdr_value       = request.wdr_value();
        if (request.has_powerline_freq())  ic.pwr_freq        = static_cast<HalIspPowerFreq>(request.powerline_freq());
        if (request.has_awb_index())       ic.awb_idx         = request.awb_index();
        // HAL set_image_config rejects awb_profile_count > 0 (read-only metadata from GET)
        ic.awb_profile_list = nullptr;
        ic.awb_profile_count = 0;
        int ret = isp_ops->set_image_config(ctx, &ic);
        if (ret < 0) {
            HAL_LOG_ERROR("CameraDaemon: set_image_config failed: %d", ret);
            return false;
        }
        cached_isp_state_ = ic;
        HAL_LOG_INFO("CameraDaemon: Image config applied: NR=%d WDR=%d PF=%d AWB=%d",
                     cached_isp_state_.noise_reduction, cached_isp_state_.wdr_value,
                     (int)cached_isp_state_.pwr_freq, cached_isp_state_.awb_idx);
    }

    // Persist the full ISP snapshot (cached_isp_state_ as updated above) so the
    // web-tuned values survive restart/deploy/OS-upgrade. Best-effort: a failure
    // logs but never alters the (already-applied) update result. update_isp_settings
    // is compiled unconditionally, but the persist helper (and proto/json_util
    // headers) live under HAS_GRPC, so the call is guarded to match (same pattern
    // as persist_privacy_mask_config / persist_transform_config).
#ifdef HAS_GRPC
    persist_isp_config();
#endif
    return true;
}

bool CameraDaemon::get_isp_config(aipc::camera::ISPConfigResponse& response) {
    // Initialize cache on first call
    if (!isp_state_initialized_) init_isp_cache();

    // Try reading from HAL first; fallback to cache if HAL read fails
    auto* isp_ops = hal_loader_ ? hal_loader_->isp() : nullptr;
    void* ctx = video_source_ ? video_source_->video_ctx() : nullptr;

    // Note: we intentionally do NOT overwrite cache from HAL on GET.
    // HAL reads live sensor values which are continuously adjusted by
    // auto-exposure/auto-white-balance algorithms. Overwriting cache
    // would cause UI values to "revert" to sensor readings that differ
    // from what the user set. The cache tracks user intent; SET updates
    // the cache only after successful HAL writes.

    // Map cached state to proto response
    const HalIspImageConfig& ic = cached_isp_state_;
    auto* cur = response.mutable_current();
    cur->set_manual_mode(ic.manual_config.manual_state);
    cur->set_brightness(ic.manual_config.brightness);
    cur->set_contrast(ic.manual_config.contrast);
    cur->set_saturation(ic.manual_config.saturation);
    cur->set_sharpness(ic.manual_config.sharpness);
    cur->set_auto_exposure(ic.exposure_config.auto_exposure);
    cur->set_backlight(ic.exposure_config.backlight);
    cur->set_exposure_time_us(ic.exposure_config.exposure_time_us);
    cur->set_gain(ic.exposure_config.gain);
    cur->set_noise_reduction(ic.noise_reduction);
    cur->set_wdr_value(ic.wdr_value);
    cur->set_powerline_freq(static_cast<int32_t>(ic.pwr_freq));
    cur->set_awb_index(ic.awb_idx);

    response.set_success(true);
    response.set_message("Read from cache");
    return true;
}

bool CameraDaemon::get_transform_config(aipc::camera::TransformConfig& config) {
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        return false;
    }

    HalMediaConfig cur;
    memset(&cur, 0, sizeof(cur));
    if (media_ops->get_current_config(media_ctx_, &cur) < 0) {
        return false;
    }

    config.set_rotation(static_cast<uint32_t>(cur.image_config.rotation_angle));
    config.set_flip(static_cast<uint32_t>(cur.image_config.flip_direction));
    config.set_dewarp(cur.image_config.dewarp);
    config.set_grayscale(cur.image_config.grayscale);
    config.set_dis(cur.image_config.dis);
    config.set_eis(cur.image_config.eis);
    return true;
}

bool CameraDaemon::set_transform_config(const aipc::camera::TransformConfig& config) {
    // Serialize the full transform (light override OR full medialib reinit +
    // post-rebuild consumer restart + frame verify). A rotation may run a
    // blocking HAL reconfigure with op_mu_ released below; without this guard a
    // second concurrent transform RPC would race the first across the released
    // window and wedge the rebuilt pipeline. Held until function exit — RAII
    // covers every return path.
    std::lock_guard<std::mutex> t_lock(transform_mu_);

    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        HAL_LOG_WARNING("CameraDaemon: Media ops not available for transform");
        return false;
    }

    // op_mu_ (exclusive) guards encoder/consumer state that the data path reads
    // under a shared lock (on_packet). Acquired here, then released around the
    // blocking HAL call and the post-rebuild resync/restart below to avoid
    // stalling the data path and the AB-BA with on_packet — same dance as
    // switch_profile.
    std::unique_lock<std::shared_mutex> lock(op_mu_);

    // Preserve privacy-mask and other image settings. The transform request
    // only owns rotation/flip/dewarp/grayscale/DIS/EIS; zero-initializing this
    // struct would silently clear the active privacy-mask configuration.
    std::lock_guard<std::mutex> privacy_lock(privacy_mask_mu_);
    HalMediaConfig cur;
    memset(&cur, 0, sizeof(cur));
    if (media_ops->get_current_config(media_ctx_, &cur) < 0) {
        HAL_LOG_ERROR("CameraDaemon: get_current_config failed for transform");
        return false;
    }

    HalMediaImageConfig ic = cur.image_config;
    const HalFlipDirection old_flip = ic.flip_direction;
    const HalRotationAngle old_rotation = ic.rotation_angle;
    const HalFlipDirection new_flip = static_cast<HalFlipDirection>(config.flip());
    remap_privacy_mask_flip(&cached_pm_items_, old_flip, new_flip);

    ic.rotation_angle = static_cast<HalRotationAngle>(config.rotation());
    ic.flip_direction = new_flip;
    ic.dewarp   = config.dewarp();
    ic.grayscale = config.grayscale();
    ic.dis      = config.dis();
    ic.eis      = config.eis();

    // Restart of the encoded-frame data consumers after a transform rebuild or
    // rotation reconfigure where consumers were pre-stopped.
    // Mirrors switch_profile's restart sequence: RTSP reset+init+local listener,
    // EncodedPublisher/FdPublisher start, autofocus rebind+start. Used on both
    // the post-reinit success path and the pre-stopped error path. Callers MUST
    // have stopped the consumers first (no callbacks fire) — runs without op_mu_
    // held (released around it), exactly like switch_profile.
    auto restart_data_consumers = [&]() {
        if (rtsp_server_ && config_.rtsp_enabled) {
            rtsp_server_.reset();
            init_rtsp();
            if (encoded_pub_) {
                auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
                encoded_pub_->add_local_listener(
                    [rtsp_weak](const std::string& sn, const HalPacketBuffer* pkt) {
                        if (auto rtsp = rtsp_weak.lock()) rtsp->on_packet(sn, pkt);
                    });
            }
        }
        if (encoded_pub_) encoded_pub_->start();
        if (fd_pub_) fd_pub_->start();
#ifdef HAS_GRPC
        if (autofocus_controller_) {
            autofocus_controller_->update_video_context(video_source_->video_ctx());
            autofocus_controller_->start();
        }
#endif
    };

    // A rotation change can rebuild/reconfigure the medialib pipeline and may
    // clear OSD overlays even when it returns HAL_OK. Stop the data consumers now,
    // under op_mu_, so the transform does not race running consumer threads
    // holding old encoder contexts — same reason switch_profile pre-stops before
    // its HAL switch. They are restarted on every successful rotation path below.
    const bool rotation_changed = (ic.rotation_angle != old_rotation);
    if (rotation_changed) {
#ifdef HAS_GRPC
        if (autofocus_controller_) {
            autofocus_controller_->stop();
            autofocus_controller_->invalidate_anchor("transform reinit");
        }
#endif
        if (encoded_pub_) encoded_pub_->stop();
        if (rtsp_server_) rtsp_server_->stop();
        if (fd_pub_) fd_pub_->stop();
    }

    // Release op_mu_ around the blocking HAL call (dynamic_change_image_config
    // -> rotation_full_reinit can take ~14s) so the data path / on_packet
    // (takes op_mu_ read) are not stalled and we avoid AB-BA. op_mu_ stays
    // released through the post-rebuild resync/restart/verify below — consumers
    // are stopped, so no callbacks fire (mirrors switch_profile).
    lock.unlock();

    int ret = media_ops->dynamic_change_image_config(media_ctx_, &ic);
    const bool full_reinit = (ret == HAL_REINIT_PERFORMED);

    if (ret < 0) {
        // Restore display-space coordinates when the transform was rejected.
        remap_privacy_mask_flip(&cached_pm_items_, new_flip, old_flip);
        HAL_LOG_ERROR("CameraDaemon: dynamic_change_image_config failed: %d", ret);
        // If we pre-stopped consumers for a rotation, restart them on the
        // (rejected/unchanged) pipeline so we don't leave the streams drained.
        if (rotation_changed) {
            restart_data_consumers();
        }
        return false;
    }

    // Cache the applied values so they can be restored after a pipeline rebuild
    // (deinit+init on stream re-enable/rollback), which otherwise resets them to
    // the medialib config file defaults.
    last_image_config_ = ic;
    have_last_image_config_ = true;

    // Persist the applied transform so it survives restart/deploy/OS-upgrade.
    // Best-effort: a failure logs but never aborts the (already-applied) apply.
    // set_transform_config is compiled unconditionally, but the persist helper
    // (and the proto/json_util headers it needs) live under HAS_GRPC, so the
    // call is guarded to match (same pattern as persist_privacy_mask_config).
#ifdef HAS_GRPC
    persist_transform_config(config);
#endif

    if (full_reinit) {
        // The HAL tore down + rebuilt the whole medialib (rotation, or the
        // Fix① flip-OOM -> rotation_full_reinit fallback). Encoder contexts /
        // output pools are brand new. If we did NOT pre-stop (flip fallback,
        // rotation_changed==false), stop now so resync does not race running
        // consumer callbacks. Then re-register encoders + reapply OSD + restart
        // consumers, else encoded output goes undrained and the buffer turnover
        // deadlocks (the rotation black-screen bug).
        if (!rotation_changed) {
#ifdef HAS_GRPC
            if (autofocus_controller_) {
                autofocus_controller_->stop();
                autofocus_controller_->invalidate_anchor("transform reinit");
            }
#endif
            if (encoded_pub_) encoded_pub_->stop();
            if (rtsp_server_) rtsp_server_->stop();
            if (fd_pub_) fd_pub_->stop();
        }

        // resync / OSD-reapply can wait for encoder callbacks that take op_mu_
        // (read) — they run with op_mu_ released (consumers stopped), matching
        // switch_profile.
        resync_encoders_from_media_pipeline();
#ifdef HAS_GRPC
        reapply_osd_config_after_pipeline_rebuild("transform reinit");
#endif
        restart_data_consumers();

        // Verify the rebuilt pipeline actually produces frames. Rotation
        // rebuilds all ISP pipelines; if the post-rebuild encoder path is dead
        // we surface the truth so the player/UI can prompt a restart rather than
        // sit on a frozen black screen. get_stream_status() also reports
        // independently. op_mu_ is not held, matching the verify window in
        // switch_profile.
        std::string primary_stream;
        if (!verify_primary_stream_frames(5000, &primary_stream)) {
            HAL_LOG_ERROR("CameraDaemon: post-transform frame verify FAILED on "
                          "'%s' after full medialib reinit — streams may need a "
                          "manual restart", primary_stream.c_str());
        } else {
            HAL_LOG_INFO("CameraDaemon: post-transform frame verify OK on '%s'",
                         primary_stream.c_str());
        }
        HAL_LOG_INFO("CameraDaemon: transform triggered full medialib reinit; consumers restarted");
    } else if (rotation_changed) {
        // Non-full rotation still passes through HAL's layout-change path, which
        // clears text/datetime/image OSD overlays so stale geometry does not reach
        // the DSP. Reapply the cached web OSD before restarting consumers, else
        // OSD text/timestamps disappear after small-resolution rotations that
        // return HAL_OK instead of HAL_REINIT_PERFORMED.
#ifdef HAS_GRPC
        reapply_osd_config_after_pipeline_rebuild("transform rotation");
#endif
        restart_data_consumers();

        // Verify the in-place rotation actually produces frames. The in-place
        // set_override_parameters path can silently wedge under a dense DSP stack
        // (dewarp + DIS/EIS): it returns HAL_OK while DSP output buffers stop
        // flowing, add_buffer() is rejected on the FE output, encoders starve and
        // /media goes black with no signal (reproduced on 93.213). Detect it here
        // so the operator sees the truth via logs / get_stream_status() instead of
        // a frozen screen. Mirrors the full-reinit branch verify above.
        {
            std::string primary_stream;
            if (!verify_primary_stream_frames(5000, &primary_stream)) {
                HAL_LOG_ERROR("CameraDaemon: post-transform frame verify FAILED on "
                              "'%s' after in-place rotation — streams may need a "
                              "manual restart", primary_stream.c_str());
            } else {
                HAL_LOG_INFO("CameraDaemon: post-transform frame verify OK on '%s'",
                             primary_stream.c_str());
            }
        }
        HAL_LOG_INFO("CameraDaemon: transform rotation completed without full reinit; consumers restarted");
    }

    HAL_LOG_INFO("CameraDaemon: Transform config applied: rot=%d flip=%d dewarp=%d gray=%d dis=%d eis=%d",
                 (int)ic.rotation_angle, (int)ic.flip_direction, ic.dewarp, ic.grayscale, ic.dis, ic.eis);
    return true;
}

bool CameraDaemon::get_privacy_mask_config(aipc::camera::PrivacyMaskConfig& config) {
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        return false;
    }

    std::lock_guard<std::mutex> privacy_lock(privacy_mask_mu_);

    HalMediaConfig cur;
    memset(&cur, 0, sizeof(cur));
    if (media_ops->get_current_config(media_ctx_, &cur) < 0) {
        return false;
    }

    const auto& pm = cur.image_config.privacy_mask_config;
    config.set_enabled(cur.image_config.privacy_mask);
    // Pack RGB into 0x00RRGGBB
    config.set_color(static_cast<uint32_t>(pm.color.r) << 16 |
                     static_cast<uint32_t>(pm.color.g) << 8 |
                     static_cast<uint32_t>(pm.color.b));
    config.set_blur_radius(pm.blur_radius);

    const uint32_t item_count = pm.items ? pm.item_count : 0;
    if (!pm.items && pm.item_count > 0) {
        HAL_LOG_WARNING("CameraDaemon: privacy mask config has %u regions but null items pointer",
                        pm.item_count);
    }

    for (uint32_t i = 0; i < item_count; ++i) {
        const auto& item = pm.items[i];
        auto* region = config.add_regions();
        region->set_id(item.id ? item.id : "");
        region->set_name(item.name ? item.name : "");
        region->set_enabled(item.is_enabled);
        for (int j = 0; j < 8; ++j) {
            if (item.points[j].x < 0.0f) break;  // sentinel: first unused slot
            region->add_points_x(item.points[j].x);
            region->add_points_y(item.points[j].y);
        }
    }

    // DPM fields are owned by camera-daemon (the media library does not persist
    // them); set_privacy_mask_config stores the effective running state here.
    // Returning them makes the web UI round-trip the toggle/labels/mode across
    // refreshes — without this, GET always reports dpm_enabled=false and the
    // toggle springs back off after reload.
    config.set_dpm_enabled(dpm_enabled_);
    config.set_dpm_labels(dpm_labels_);
    // Reconstruct the render-mode string from the atomic<int> (the frontend
    // hot-path stores/reads an int; only the API boundary uses the string).
    config.set_dpm_mode(dpm_render_mode_to_string(dpm_render_mode_.load(std::memory_order_relaxed)));
    config.set_dpm_color(dpm_color_.load(std::memory_order_relaxed));
    return true;
}

bool CameraDaemon::set_privacy_mask_config(const aipc::camera::PrivacyMaskConfig& config) {
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        HAL_LOG_WARNING("CameraDaemon: Media ops not available for privacy mask");
        return false;
    }

    std::lock_guard<std::mutex> privacy_lock(privacy_mask_mu_);

    // Get current image config to preserve transform fields
    HalMediaConfig cur;
    memset(&cur, 0, sizeof(cur));
    if (media_ops->get_current_config(media_ctx_, &cur) < 0) {
        HAL_LOG_ERROR("CameraDaemon: get_current_config failed for privacy mask");
        return false;
    }

    HalMediaImageConfig ic = cur.image_config;

    // STATIC privacy-mask arming ONLY. The medialib master switch + the static
    // color/blur_radius below drive the static polygon blender exclusively.
    // Dynamic masking is fully DECOUPLED: DPM never touches this config — it
    // bakes ROIs onto the live pre-encode frame via HAL_DRAW_OPS in the frontend
    // lambda, with its own style (dpm_render_mode_ / dpm_color_). The two
    // mechanisms never share state, so toggling DPM no longer raises the static
    // master switch (the original "DPM tied to the privacy-mask toggle" coupling).
    if (config.enabled()) {
        ic.digital_zoom = false;
        ic.digital_zoom_value = 0;
    }
    ic.privacy_mask = config.enabled();

    // Unpack color from 0x00RRGGBB
    ic.privacy_mask_config.color.r = static_cast<uint8_t>((config.color() >> 16) & 0xFF);
    ic.privacy_mask_config.color.g = static_cast<uint8_t>((config.color() >> 8) & 0xFF);
    ic.privacy_mask_config.color.b = static_cast<uint8_t>(config.color() & 0xFF);
    ic.privacy_mask_config.blur_radius = config.blur_radius();

    // Convert proto regions → HalPrivacyMaskItem[]
    // Store in cached_pm_items_ so pointers remain valid after this call returns
    cached_pm_items_.clear();
    cached_pm_items_.reserve(config.regions_size());

    for (int i = 0; i < config.regions_size(); ++i) {
        const auto& region = config.regions(i);
        HalPrivacyMaskItem item;
        memset(&item, 0, sizeof(item));
        item.id = nullptr;   // will be set from cached strings below
        item.name = nullptr;
        item.is_enabled = region.enabled();

        int pt_count = std::min(region.points_x_size(), region.points_y_size());
        pt_count = std::min(pt_count, 8);
        for (int j = 0; j < pt_count; ++j) {
            item.points[j].x = region.points_x(j);
            item.points[j].y = region.points_y(j);
        }
        // Sentinel: mark first unused slot
        for (int j = pt_count; j < 8; ++j) {
            item.points[j].x = -1.0f;
            item.points[j].y = -1.0f;
        }
        cached_pm_items_.push_back(item);
    }

    // Now set id/name pointers from member string storage. HAL keeps shallow
    // pointers in its current config, so thread-local/request-owned strings are
    // not safe after the RPC worker returns.
    cached_pm_ids_.clear();
    cached_pm_names_.clear();
    cached_pm_ids_.reserve(config.regions_size());
    cached_pm_names_.reserve(config.regions_size());
    for (int i = 0; i < config.regions_size(); ++i) {
        cached_pm_ids_.push_back(config.regions(i).id());
        cached_pm_names_.push_back(config.regions(i).name());
        cached_pm_items_[i].id = cached_pm_ids_.back().c_str();
        cached_pm_items_[i].name = cached_pm_names_.back().c_str();
    }

    ic.privacy_mask_config.items = cached_pm_items_.empty() ? nullptr : cached_pm_items_.data();
    ic.privacy_mask_config.item_count = static_cast<uint32_t>(cached_pm_items_.size());

    // ---- Dynamic Privacy Mask (DECOUPLED from the static blender) ----
    // DPM is no longer armed through the medialib blender. It runs its own
    // detector(s) in dpm_worker_ and bakes ROIs onto the live pre-encode frame
    // via HAL_DRAW_OPS in the frontend lambda, with its own style
    // (dpm_render_mode_ / dpm_color_). So we do NOT use
    // privacy_mask_config.dynamic_enabled / masked_labels / dilation_size to
    // drive DPM — those stay static-only. We keep them explicitly OFF here so
    // the blender never re-enters dynamic mode, and only derive the worker
    // label input below; the worker start is gated after the config apply.
    bool dpm_wanted = config.dpm_enabled();
    bool dpm_restricted = false;
    // Empty labels are a VALID state (#7): DPM armed with no targets → idle
    // worker (runs, publishes no mask). The web hint "empty = idle" relies on
    // this. Do NOT force a "person" default — that would silently mask people
    // the user explicitly deselected.
    std::string dpm_labels_csv = config.dpm_labels();
    ic.privacy_mask_config.dynamic_enabled = false;
    ic.privacy_mask_config.num_masked_labels = 0;
    ic.privacy_mask_config.dilation_size = 0;
    // Master switch is STATIC-only (set above); DPM no longer raises it.

    int ret = media_ops->dynamic_change_image_config(media_ctx_, &ic);
    if (ret < 0) {
        if (ret == HAL_ERR_PROFILE_RESTRICTED) {
            // Thermal/power restriction blocked the (static-only) image config
            // change. DPM runs on its own draw path independent of this config,
            // but we still gate the worker off under restriction to honor the
            // device limit (NPU inference would worsen thermal pressure).
            HAL_LOG_WARNING("CameraDaemon: image config restricted (thermal/profile): %d", ret);
            dpm_restricted = true;
        } else {
            HAL_LOG_ERROR("CameraDaemon: dynamic_change_image_config failed for privacy mask: %d", ret);
            stop_dpm_worker();  // tear down regardless — config did not apply
            return false;
        }
    }

    last_image_config_ = ic;
    have_last_image_config_ = true;

    // #4 Restart dedup: the worker only needs to (re)start when the ARMED state
    // or the target LABELS change. mode/color/static-region edits take effect via
    // the atomics + the config apply above on the next frame — no HEF reload
    // (~1-2s stall) needed. dpm_armed_/dpm_armed_labels_ track the worker we
    // actually built, so a no-op re-PUT (e.g. a mode/color slider drag, or a
    // static-region edit) does NOT restart inference.
    bool start_ok = false;
    const bool need_worker = dpm_wanted && !dpm_restricted;
    if (need_worker) {
        const bool same_armed = dpm_armed_ && dpm_armed_labels_ == dpm_labels_csv;
        if (same_armed) {
            // Already running with exactly these labels — keep it. A prior FAILED
            // start (dpm_armed_ == false) is not "same", so a re-toggle retries.
            start_ok = true;
        } else {
            start_ok = start_dpm_worker(config);
        }
    } else if (dpm_armed_) {
        stop_dpm_worker();
    }

    // Persist the effective DPM state so get_privacy_mask_config reports what is
    // actually running. dpm_enabled_ is true only if a worker is live (wanted AND
    // not restricted AND start succeeded / was already running with these labels).
    // Empty labels are valid → dpm_labels_ stores them verbatim (idle worker).
    dpm_enabled_ = start_ok;
    dpm_armed_ = start_ok;
    dpm_armed_labels_ = start_ok ? dpm_labels_csv : std::string();
    dpm_labels_ = dpm_labels_csv;
    dpm_render_mode_.store(dpm_render_mode_from_string(config.dpm_mode()),
                           std::memory_order_relaxed);
    dpm_color_.store(config.dpm_color(), std::memory_order_relaxed);

    HAL_LOG_INFO("CameraDaemon: Privacy mask config applied: enabled=%d blur=%d regions=%zu dpm=%d",
                 ic.privacy_mask, ic.privacy_mask_config.blur_radius, cached_pm_items_.size(),
                 dpm_enabled_ ? 1 : 0);
#ifdef HAS_GRPC
    // Best-effort disk mirror so this config survives restart/deploy/OS-upgrade.
    // set_privacy_mask_config is compiled unconditionally, but the persist helper
    // (and the proto/json_util headers it needs) live under HAS_GRPC, so the call
    // is guarded to match. Failure only logs — it must never alter the apply result.
    persist_privacy_mask_config(config);
#endif
    return true;
}

bool CameraDaemon::start_dpm_worker(const aipc::camera::PrivacyMaskConfig& config) {
    // Require the DPM ops tables (inference + postprocess + DSP + frame_buffer).
    // If the HAL doesn't expose them, DPM cannot run — log and bail (no crash).
    if (!hal_loader_ || !hal_loader_->has_inference() || !hal_loader_->has_postprocess() ||
        !hal_loader_->has_dsp() || !hal_loader_->has_frame_buffer()) {
        HAL_LOG_WARNING("CameraDaemon: DPM requested but HAL inference/postprocess/dsp/frame_buffer ops unavailable");
        return false;
    }
    if (!frame_router_ || !media_ctx_) {
        HAL_LOG_WARNING("CameraDaemon: DPM requested but frame_router/media_ctx unavailable");
        return false;
    }

    // Parse the requested label set (CSV). Only build detector specs for the
    // labels the user selected, so the NPU is never wasted on unused models.
    // Labels outside the supported four (person/face/vehicle/license_plate) are
    // ignored. person → linknet segmentation (silhouette); vehicle/face → the
    // 4-class yolov8n detector (it emits a DIRECT face bbox — no derivation,
    // no dedicated face HEF); license_plate → its own tiny_yolov4 detector,
    // decoded in-house (raw uint16 grid, see dpm_worker.cpp decode_plate_grid).
    std::vector<std::string> labels;
    {
        std::string token;
        // Empty label set is valid (#7) → idle worker. Do NOT default to "person":
        // that would silently mask people the user explicitly deselected.
        std::istringstream ls(config.dpm_labels());
        while (std::getline(ls, token, ',')) {
            auto a = token.find_first_not_of(" \t");
            auto b = token.find_last_not_of(" \t");
            if (a == std::string::npos) continue;
            labels.push_back(token.substr(a, b - a + 1));
        }
    }
    auto has_label = [&](const std::string& s) {
        for (const auto& l : labels) if (l == s) return true;
        return false;
    };

    // HEF / postproc-JSON paths match the 93.72 model layout
    // (/data/aipc-data/models/<category>/). These are deployment-specific; a
    // missing/unsupported HEF is skipped GRACEFULLY by DpmWorker::init_sessions
    // (log + continue) — so person/vehicle/plate still ship even if the face HEF
    // is absent on this device. TODO: drive from camera-daemon.yaml once stable.
    std::vector<DpmWorker::DetectorSpec> specs;

    // person → linknet semantic segmentation (real per-pixel silhouette).
    // is_seg selects the SEG postproc path (linknet_post is the default Hailo-15
    // plugin for the SEG type — no JSON required). keep_labels is empty: seg has
    // no class-name filter; the foreground threshold (cid==1||cid>=128) lives
    // inside run_segmenter, verbatim from ai_example_v2.cpp:1525. If this HEF
    // is absent on the device, init skips it gracefully and the coco spec below
    // still ships a person bbox fallback.
    if (has_label("person")) {
        DpmWorker::DetectorSpec s;
        s.name = "person_seg";
        s.hef = "/data/aipc-data/models/segmentation/linknet_mbv1_ss_dpm_256.hef";
        s.is_seg = true;
        specs.push_back(std::move(s));
    }

    // One yolov8n detector for person (bbox fallback) / vehicle / face. This HEF
    // is a CUSTOM 4-class model — person/vehicle/face/license_plate — NOT coco-80
    // (scripts/download_models.sh "[7/13] ... 4-class person/vehicle/face";
    // hailortcli parse-hef reports "number of classes: 4"; neoruntime-apps/showcases/model-showcase
    // main.py:1939 filters its output by label=="face"). It emits a DIRECT face
    // bbox — no head-rect derivation, no missing yolov5_personface.hef. For person
    // this is the bbox FALLBACK only (the real silhouette comes from person_seg
    // above). The postproc JSON (labels + label_offset=1) MUST exist at the path
    // below so the postprocess populates d.label ("person"/"vehicle"/"face");
    // without it d.label is empty and keep_labels string-match never fires.
    if (has_label("person") || has_label("vehicle") || has_label("face")) {
        DpmWorker::DetectorSpec s;
        s.name = "yolov8n";
        s.hef = "/data/aipc-data/models/detection/hailo_yolov8n_384_640.hef";
        s.post_json = "/data/aipc-data/models/detection/hailo_yolov8n_384_640.json";
        if (has_label("person"))  s.keep_labels.push_back("person");
        if (has_label("vehicle")) s.keep_labels.push_back("vehicle");
        if (has_label("face"))    s.keep_labels.push_back("face");
        specs.push_back(std::move(s));
    }
    if (has_label("license_plate")) {
        DpmWorker::DetectorSpec s;
        s.name = "plate";
        s.hef = "/data/aipc-data/models/detection/tiny_yolov4_license_plates.hef";
        // tiny_yolov4 emits a raw uint16 YOLO grid (NO on-chip NMS) that the HAL
        // NMS postproc cannot decode — and its JSON was never generated by
        // download_models.sh anyway. is_grid_det makes DpmWorker decode the raw
        // output tensors itself (decode_plate_grid in dpm_worker.cpp), bypassing
        // postproc entirely. keep_labels empty => keep every detection (only plates).
        s.is_grid_det = true;
        specs.push_back(std::move(s));
    }
    // Empty specs is VALID (#7): DPM armed with no recognized target → idle
    // worker (runs, no inference, publishes no mask). The four supported labels
    // are person/face/vehicle/license_plate; anything else (or an empty set) →
    // idle. Do NOT fail here — the toggle stays ON and get_latest() == nullptr.

    DpmWorker::Config cfg;
    // Clean-capture model (#1): the worker is NOT a FrameRouter subscriber. The
    // frontend lambda calls offer_frame() on the GStreamer streaming thread with
    // the clean pre-bake MAIN frame; inference never sees a frame it masked. No
    // frame_router / stream Config fields anymore (DpmWorker::Config dropped them).
    cfg.infer_ops = hal_loader_->inference();
    cfg.post_ops = hal_loader_->postprocess();
    cfg.dsp_ops = hal_loader_->dsp();
    cfg.fb_ops = hal_loader_->frame_buffer();
    cfg.detectors = std::move(specs);

    // Build + start OUTSIDE dpm_mu_ (HEF load is ~1-2s; the frontend draw lambda
    // must never block on it). Only the pointer swap is locked.
    auto worker = std::make_shared<DpmWorker>();
    if (!worker->start(cfg)) {
        // No detector loaded (all HEFs missing) or session-create failed —
        // typically transient NPU contention (ai-runtime / model-showcase holding
        // the same HEFs at this instant). Return false so the caller persists
        // dpm_enabled_=false: the UI toggle then honestly reverts to OFF instead
        // of showing ON with no mask.
        HAL_LOG_ERROR("CameraDaemon: DPM worker start failed (no detector loaded / HEF contention) — "
                      "toggle will report OFF; re-toggle to retry once the NPU is free");
        return false;
    }
    std::string loaded;
    for (const auto& d : cfg.detectors) {
        if (!loaded.empty()) loaded += ",";
        loaded += d.name;
    }
    HAL_LOG_INFO("CameraDaemon: DPM worker started (detectors=%s%s)",
                 loaded.c_str(),
                 cfg.detectors.empty() ? " [IDLE]" : "");

    std::shared_ptr<DpmWorker> old;
    {
        std::unique_lock<std::shared_mutex> lk(dpm_mu_);
        old = std::move(dpm_worker_);
        dpm_worker_ = worker;
    }
    if (old) old->stop();  // stop previous outside the lock
    return true;
}

void CameraDaemon::stop_dpm_worker() {
    std::shared_ptr<DpmWorker> old;
    {
        std::unique_lock<std::shared_mutex> lk(dpm_mu_);
        old = std::move(dpm_worker_);
    }
    if (old) {
        old->stop();  // join worker thread + destroy sessions, outside the lock
        HAL_LOG_INFO("CameraDaemon: DPM worker stopped");
    }
}

void CameraDaemon::restore_image_config_if_cached() {
    if (!have_last_image_config_) {
        return;
    }
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_ || !media_ops->dynamic_change_image_config) {
        HAL_LOG_WARNING("CameraDaemon: cannot restore image config — media ops unavailable");
        return;
    }
    int ret = media_ops->dynamic_change_image_config(media_ctx_, &last_image_config_);
    if (ret < 0) {
        HAL_LOG_ERROR("CameraDaemon: restore image config after reinit failed: %d", ret);
        return;
    }
    HAL_LOG_INFO("CameraDaemon: Restored image config after reinit: rot=%d flip=%d dewarp=%d gray=%d dis=%d eis=%d",
                 (int)last_image_config_.rotation_angle, (int)last_image_config_.flip_direction,
                 last_image_config_.dewarp, last_image_config_.grayscale,
                 last_image_config_.dis, last_image_config_.eis);
}

void CameraDaemon::bind_video_source_callbacks() {
    if (!video_source_) {
        return;
    }

    // In FROM_MEDIA mode, translate media pipeline stream names to config names.
    for (auto& slot : video_source_->streams()) {
        std::string dispatch_name = slot.name;
        auto vnit = video_name_map_.find(slot.name);
        if (vnit != video_name_map_.end()) dispatch_name = vnit->second;

        video_source_->set_frame_callback(
            slot.name,
            [this, dispatch_name](const std::string&, HalFrameBuffer* frame) {
                handle_video_frame_for_routing(dispatch_name, frame);
            });
    }
}

void CameraDaemon::handle_video_frame_for_routing(const std::string& dispatch_name,
                                                  HalFrameBuffer* frame) {
    if (!frame) {
        return;
    }

    // Dynamic Privacy Mask: bake the worker-produced bytemask onto
    // the LIVE encoder-input frame via HAL_DRAW_OPS BEFORE
    // on_frame_arrived routes it to the encoders. The bytemask is a
    // single frame-resolution silhouette (person seg + det bboxes
    // OR'd together) computed on the DPM worker thread. This is
    // intentionally DECOUPLED from the static privacy-mask blender:
    // the blender owns the static polygons + static color/style,
    // while DPM uses draw_mask/draw_mosaic with its OWN style
    // (dpm_render_mode_ + dpm_color_). The two mechanisms never
    // share state, so they compose even when both are on, with
    // visibly different styles, and toggling one never disturbs the
    // other. Inference + bytemask build run on the DPM worker
    // thread; this hot path does only a shared_ptr copy + a single
    // draw_mask (overlay) or bounded draw_mosaic calls over the
    // worker-precomputed cells (mosaic/blur) — never blocks on
    // inference. Draw runs on each stream's GStreamer streaming
    // thread; the bytemask is owned by the worker snapshot, so there
    // are no cross-stream races and no per-thread scratch needed.
    // #1 Clean Frame Path: feed the CLEAN (pre-bake) frame to the worker
    // BEFORE any draw below mutates it. Inference must never run on a
    // frame it has already masked — the hidden person would feed back
    // into its own detection. Only the MAIN stream is offered: it is the
    // highest-res feed (best detail for inference) and the bytemask is
    // built at main resolution; every other stream downscales the
    // published mask to its own dims in the render block below. offer is
    // self-throttled + drop-on-pending inside the worker, so this never
    // blocks the streaming thread beyond the bounded DSP resize(s).
    if (dispatch_name == "main") {
        std::shared_ptr<DpmWorker> dpm_capture;
        {
            std::shared_lock<std::shared_mutex> lk(dpm_mu_);
            dpm_capture = dpm_worker_;
        }
        if (dpm_capture && dpm_capture->is_running()) {
            dpm_capture->offer_frame(frame);
        }
    }

    std::shared_ptr<DpmWorker> dpm;
    {
        std::shared_lock<std::shared_mutex> lk(dpm_mu_);
        dpm = dpm_worker_;
    }
    if (dpm && dpm->is_running() && hal_loader_ && hal_loader_->has_draw()) {
        auto state = dpm->get_latest();
        // #6 Per-stream render: the bytemask is built at MAIN-stream
        // resolution. EVERY stream renders it — main is an exact match
        // (zero-copy straight off the snapshot); sub/third DOWNSCALE it
        // to this frame's dims (overlay nearest-neighbors into a
        // per-thread scratch; mosaic/blur scales cell coords). No stream
        // is left unmasked. A mask only exists while main is configured
        // and running (it's the sole feed offered to the worker).
        if (state && !state->restricted &&
            state->frame_w > 0 && state->frame_h > 0 &&
            !state->mask.empty()) {
            auto* draw_ops = hal_loader_->draw();
            const int mode = dpm_render_mode_.load(std::memory_order_relaxed);
            const uint32_t dpm_color = dpm_color_.load(std::memory_order_relaxed);
            const bool exact = (state->frame_w == frame->width &&
                                state->frame_h == frame->height);

            if (mode == kDpmRenderOverlay) {
                // One draw_mask over a frame_w*frame_h bytemask. Exact
                // match reuses the worker snapshot directly; otherwise
                // nearest-neighbor downscale into a per-thread scratch
                // (the snapshot is immutable → can't downscale in place).
                const uint8_t* mask_ptr;
                uint32_t mw, mh;
                if (exact) {
                    mask_ptr = state->mask.data();
                    mw = state->frame_w;
                    mh = state->frame_h;
                } else {
                    thread_local std::vector<uint8_t> scratch;
                    scratch.assign(static_cast<size_t>(frame->width) * frame->height, 0);
                    const uint32_t fw = state->frame_w;
                    const uint32_t fh = state->frame_h;
                    for (uint32_t y = 0; y < frame->height; ++y) {
                        const uint32_t sy = (y * fh) / frame->height;
                        const uint8_t* srow = &state->mask[static_cast<size_t>(sy) * fw];
                        uint8_t* drow = &scratch[static_cast<size_t>(y) * frame->width];
                        for (uint32_t x = 0; x < frame->width; ++x) {
                            drow[x] = srow[(x * fw) / frame->width];
                        }
                    }
                    mask_ptr = scratch.data();
                    mw = frame->width;
                    mh = frame->height;
                }
                HalDrawMask m{};
                m.x = 0; m.y = 0;
                m.width = mw;
                m.height = mh;
                // draw_mask reads mask_data as 0/255 only — const_cast
                // is safe on the immutable snapshot / thread scratch.
                m.mask_data = const_cast<uint8_t*>(mask_ptr);
                m.color = hal_color_rgb(
                    static_cast<uint8_t>((dpm_color >> 16) & 0xFF),
                    static_cast<uint8_t>((dpm_color >> 8) & 0xFF),
                    static_cast<uint8_t>(dpm_color & 0xFF));
                m.alpha = 1.0f;
                draw_ops->draw_mask(frame, &m);
            } else {
                // mosaic/blur: iterate the worker-precomputed occupied
                // cells (main-resolution). Each cell is one bounded
                // draw_mosaic call; coords scale to this stream's dims
                // when it isn't main (block_size>0 = mosaic, 0 = blur).
                const uint32_t block =
                    (mode == kDpmRenderBlur) ? 0u : 16u;
                const float sx = exact ? 1.0f
                    : static_cast<float>(frame->width) / state->frame_w;
                const float sy = exact ? 1.0f
                    : static_cast<float>(frame->height) / state->frame_h;
                for (const auto& cell : state->mosaic_cells) {
                    HalDrawMosaic mz{};
                    mz.x = static_cast<int32_t>(cell.x * sx);
                    mz.y = static_cast<int32_t>(cell.y * sy);
                    mz.width = static_cast<uint32_t>(cell.w * sx + 0.5f);
                    mz.height = static_cast<uint32_t>(cell.h * sy + 0.5f);
                    mz.block_size = block;
                    draw_ops->draw_mosaic(frame, &mz);
                }
            }
        }
    }

    if (frame_router_) {
        frame_router_->on_frame_arrived(dispatch_name, frame);
    }
}

bool CameraDaemon::update_encoder_config(const std::string& stream_name, uint32_t bitrate_bps, uint32_t framerate, uint32_t gop) {
    std::unique_lock<std::mutex> reconfig_lock(pipeline_reconfig_mu_, std::try_to_lock);
    if (!reconfig_lock.owns_lock()) {
        HAL_LOG_WARNING("CameraDaemon: Encoder/pipeline reconfiguration already in progress");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);
    if (!encoder_mgr_) {
        HAL_LOG_ERROR("CameraDaemon: Encoder manager not initialized");
        return false;
    }

    // Find the encoder config for the requested stream
    bool target_found = false;
    for (const auto& ec : config_.encoders) {
        if (ec.stream_name == stream_name) {
            target_found = true;
            break;
        }
    }

    if (!target_found) {
        HAL_LOG_ERROR("CameraDaemon: Stream '%s' not found in encoder config", stream_name.c_str());
        return false;
    }

    // In FROM_MEDIA mode, encoder_mgr uses media pipeline names — translate
    std::string enc_name = stream_name;
    for (auto& [media_name, config_name] : encoder_name_map_) {
        if (config_name == stream_name) {
            enc_name = media_name;
            break;
        }
    }

    // Release lock before calling encoder_mgr methods — the underlying
    // medialib set_override_parameters() can block when the pipeline is
    // running.  Holding op_mu_ starves RTSP/AI-Overlay/OSD operations.
    lock.unlock();

    bool success = true;
    bool bitrate_applied = false;
    bool framerate_applied = false;
    bool gop_applied = false;

    if (bitrate_bps > 0) {
        int ret = encoder_mgr_->set_bitrate(enc_name, bitrate_bps);
        if (ret == 0) {
            HAL_LOG_INFO("CameraDaemon: Set bitrate %u for encoder %s",
                         bitrate_bps, stream_name.c_str());
            bitrate_applied = true;
        } else {
            HAL_LOG_WARNING("CameraDaemon: Failed to set bitrate for %s", stream_name.c_str());
            success = false;
        }
    }

    if (framerate > 0) {
        int ret = encoder_mgr_->set_framerate(enc_name, framerate);
        if (ret == 0) {
            HAL_LOG_INFO("CameraDaemon: Set framerate %u for encoder %s",
                         framerate, stream_name.c_str());
            framerate_applied = true;
        } else {
            HAL_LOG_WARNING("CameraDaemon: Failed to set framerate for %s", stream_name.c_str());
            success = false;
        }
    }

    if (gop > 0) {
        int ret = encoder_mgr_->set_gop(enc_name, gop);
        if (ret == 0) {
            HAL_LOG_INFO("CameraDaemon: Set GOP %u for encoder %s",
                         gop, stream_name.c_str());
            gop_applied = true;
        } else {
            HAL_LOG_WARNING("CameraDaemon: Failed to set GOP for %s", stream_name.c_str());
            success = false;
        }
    }

    lock.lock();
    for (auto& ec : config_.encoders) {
        if (ec.stream_name != stream_name) continue;
        if (bitrate_applied) ec.bitrate = bitrate_bps;
        if (framerate_applied) ec.fps = framerate;
        if (gop_applied) ec.gop = gop;
        break;
    }

    return success && (bitrate_bps > 0 || framerate > 0 || gop > 0);
}

#ifdef HAS_GRPC
bool CameraDaemon::reconfigure_encoder(const aipc::camera::EncoderReconfigRequest& request,
                                          aipc::camera::EncoderReconfigResponse& response) {
    std::unique_lock<std::mutex> reconfig_lock(pipeline_reconfig_mu_, std::try_to_lock);
    if (!reconfig_lock.owns_lock()) {
        HAL_LOG_WARNING("CameraDaemon: Encoder/pipeline reconfiguration already in progress");
        response.set_success(false);
        response.set_message("Encoder/pipeline reconfiguration already in progress");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);
    const std::string& stream_name = request.stream_name();

    // Find the encoder config for the requested stream
    int target_idx = -1;
    for (size_t i = 0; i < config_.encoders.size(); ++i) {
        if (config_.encoders[i].stream_name == stream_name) {
            target_idx = static_cast<int>(i);
            break;
        }
    }

    if (target_idx < 0) {
        HAL_LOG_ERROR("CameraDaemon: Stream '%s' not found in encoder config", stream_name.c_str());
        response.set_success(false);
        response.set_message("Stream not found: " + stream_name);
        return false;
    }
    EncoderCfg target_before = config_.encoders[target_idx];

    // --- FROM_MEDIA mode: use HAL override (no encoder destroy/recreate) ---
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (media_ops && media_ctx_ && media_ops->override_stream_params) {
        // Resolve pipeline stream id via encoder_name_map_ (reverse: "main" → "sink0")
        std::string pipeline_id = stream_name;
        for (const auto& kv : encoder_name_map_) {
            if (kv.second == stream_name) {
                pipeline_id = kv.first;
                break;
            }
        }

        HalStreamOverride ov = {};
        snprintf(ov.stream_id, sizeof(ov.stream_id), "%s", pipeline_id.c_str());

        bool has_dimension_change = false;
        if (request.width() > 0 || request.height() > 0) {
            ov.encoder_width = (request.width() > 0) ? request.width() : target_before.width;
            ov.encoder_height = (request.height() > 0) ? request.height() : target_before.height;
            ov.input_width = ov.encoder_width;
            ov.input_height = ov.encoder_height;
            has_dimension_change = (ov.encoder_width != target_before.width || ov.encoder_height != target_before.height);
        }
        if (request.fps() > 0) {
            ov.encoder_framerate = request.fps();
            ov.input_framerate = request.fps();
        }
        if (request.bitrate_bps() > 0)
            ov.encoder_bitrate = request.bitrate_bps();
        if (request.gop() > 0)
            ov.encoder_gop = request.gop();
        if (!request.codec().empty())
            snprintf(ov.encoder_codec, sizeof(ov.encoder_codec), "%s", request.codec().c_str());

        // Dimension/fps/codec changes require pipeline stop-start to avoid encoder race condition.
        // Bitrate/GOP-only changes can be applied hot (media library handles them internally).
        bool need_pipeline_restart = has_dimension_change ||
                                     (request.fps() > 0 && request.fps() != target_before.fps) ||
                                     !request.codec().empty();
        std::vector<std::string> feed_stream_ids;
        if (encoder_auto_feed_enabled_.load()) {
            feed_stream_ids.reserve(config_.encoders.size());
            for (const auto& ec : config_.encoders) {
                if (!ec.enabled) continue;
                // Collect pipeline names (sink0/sink1/...) for auto-feed restore
                std::string pid = ec.stream_name;
                for (const auto& [media_name, config_name] : encoder_name_map_) {
                    if (config_name == ec.stream_name) {
                        pid = media_name;
                        break;
                    }
                }
                feed_stream_ids.push_back(pid);
            }
        }

        // Release op_mu_ before any HAL calls that may block on pipeline
        // stop/start — those wait for GStreamer callbacks that hold priv->mutex
        // and call output_fn_ which takes op_mu_ (read).  Same AB-BA deadlock
        // pattern as remove_stream / reconfigure_pipeline.

        // Stop AF before pipeline restart — the video source goes away during
        // stop/start and any in-flight AF job would time out or read stale frames.
        if (need_pipeline_restart && autofocus_controller_) {
            autofocus_controller_->stop();
            autofocus_controller_->invalidate_anchor("encoder pipeline reconfigured");
        }

        lock.unlock();

        if (need_pipeline_restart && media_ops->stop) {
            HAL_LOG_INFO("CameraDaemon: Stopping pipeline for encoder reconfigure (dimension/fps/codec change)");
            int stop_ret = media_ops->stop(media_ctx_);
            if (stop_ret < 0) {
                HAL_LOG_ERROR("CameraDaemon: Pipeline stop failed before reconfigure: %d", stop_ret);
                response.set_success(false);
                response.set_message("Pipeline stop failed: " + std::to_string(stop_ret));
                return false;
            }
        }

        HalStreamOverrideBatch batch = {};
        batch.streams = &ov;
        batch.stream_count = 1;

        auto start_time = std::chrono::steady_clock::now();
        int ret = media_ops->override_stream_params(media_ctx_, &batch);
        auto end_time = std::chrono::steady_clock::now();
        uint32_t interrupt_ms = static_cast<uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

        if (ret < 0) {
            HAL_LOG_ERROR("CameraDaemon: override_stream_params for %s failed: %d", stream_name.c_str(), ret);
            // Attempt pipeline restart even on failure (it was stopped)
            if (need_pipeline_restart && media_ops->start) {
                int sr = media_ops->start(media_ctx_);
                if (sr < 0) {
                    HAL_LOG_ERROR("CameraDaemon: Pipeline restart after override failure also failed: %d", sr);
                }
            }
            // Attempt to recover AF after failed override
            if (need_pipeline_restart && autofocus_controller_) {
                autofocus_controller_->update_video_context(video_source_->video_ctx());
                autofocus_controller_->start();
            }
            response.set_success(false);
            response.set_message("Override failed: " + std::to_string(ret));
            response.set_interrupt_ms(interrupt_ms);
            return false;
        }

        if (need_pipeline_restart && media_ops->start) {
            HAL_LOG_INFO("CameraDaemon: Restarting pipeline after encoder reconfigure");
            int sr = media_ops->start(media_ctx_);
            if (sr < 0) {
                HAL_LOG_ERROR("CameraDaemon: Pipeline restart failed: %d", sr);
                // Attempt to recover AF after failed pipeline restart
                if (autofocus_controller_) {
                    autofocus_controller_->update_video_context(video_source_->video_ctx());
                    autofocus_controller_->start();
                }
                response.set_success(false);
                response.set_message("Pipeline restart failed: " + std::to_string(sr));
                response.set_interrupt_ms(interrupt_ms);
                return false;
            }
            // Some HAL/MediaLibrary paths reset feed mode after stop/start.
            // Restore auto-feed so encoded sockets continue producing packets.
            if (encoder_auto_feed_enabled_.load() && media_ops->set_encoder_auto_feed) {
                int ar = media_ops->set_encoder_auto_feed(media_ctx_, true);
                if (ar < 0) {
                    HAL_LOG_WARNING("CameraDaemon: set_encoder_auto_feed(true) after reconfigure failed: %d", ar);
                } else {
                    HAL_LOG_INFO("CameraDaemon: Restored encoder auto-feed after reconfigure");
                    if (media_ops->set_encoder_auto_feed_for_stream) {
                        for (const auto& sid : feed_stream_ids) {
                            int sr2 = media_ops->set_encoder_auto_feed_for_stream(media_ctx_, sid.c_str(), true);
                            if (sr2 < 0) {
                                HAL_LOG_WARNING("CameraDaemon: set_encoder_auto_feed_for_stream(%s,true) failed after reconfigure: %d",
                                                sid.c_str(), sr2);
                            }
                        }
                    }
                }
            }
            // MediaLibrary may reallocate codec contexts after restart.
            // Rebind encoder manager contexts to avoid stale handles.
            resync_encoders_from_media_pipeline();
            reapply_osd_config_after_pipeline_rebuild("encoder reconfigure restart");

            // Refresh AF video context — the stop/start cycle may have
            // reallocated internal video contexts.
            if (autofocus_controller_) {
                autofocus_controller_->update_video_context(video_source_->video_ctx());
                autofocus_controller_->start();
            }
        }

        // Re-lock and update stored config by stream name to avoid using a stale pointer
        // after unlock/medialib calls.
        lock.lock();
        for (auto& ec : config_.encoders) {
            if (ec.stream_name != stream_name) continue;
            if (request.width() > 0) ec.width = request.width();
            if (request.height() > 0) ec.height = request.height();
            if (!request.codec().empty()) ec.codec = request.codec();
            if (request.bitrate_bps() > 0) ec.bitrate = request.bitrate_bps();
            if (request.fps() > 0) ec.fps = request.fps();
            if (request.gop() > 0) ec.gop = request.gop();
            break;
        }
        lock.unlock();

        HAL_LOG_INFO("CameraDaemon: Reconfigured encoder %s via HAL override (interrupt: %ums)",
                     stream_name.c_str(), interrupt_ms);

        response.set_success(true);
        response.set_message("Reconfigured via HAL override");
        response.set_interrupt_ms(interrupt_ms);
        return true;
    }

    // --- Standalone mode: destroy + recreate encoder ---
    if (!encoder_mgr_) {
        HAL_LOG_ERROR("CameraDaemon: Encoder manager not initialized");
        response.set_success(false);
        response.set_message("Encoder manager not initialized");
        return false;
    }

    HalCodecConfig new_config = {};
    new_config.type = HAL_CODEC_TYPE_HW;
    if (request.codec() == "h265") {
        new_config.packet_type = HAL_PACKET_TYPE_H265;
    } else {
        new_config.packet_type = HAL_PACKET_TYPE_H264;
    }
    new_config.width = (request.width() > 0) ? request.width() : target_before.width;
    new_config.height = (request.height() > 0) ? request.height() : target_before.height;
    new_config.framerate = (request.fps() > 0) ? request.fps() : target_before.fps;
    new_config.bitrate = (request.bitrate_bps() > 0) ? request.bitrate_bps() : target_before.bitrate;
    new_config.gop_size = (request.gop() > 0) ? request.gop() : target_before.gop;

    uint32_t interrupt_ms = 0;
    int ret = encoder_mgr_->reconfigure(stream_name, new_config, &interrupt_ms);

    if (ret < 0) {
        HAL_LOG_ERROR("CameraDaemon: Reconfigure encoder %s failed: %d", stream_name.c_str(), ret);
        response.set_success(false);
        response.set_message("Reconfigure failed: " + std::to_string(ret));
        return false;
    }

    // Update stored config
    if (request.width() > 0) config_.encoders[target_idx].width = request.width();
    if (request.height() > 0) config_.encoders[target_idx].height = request.height();
    if (!request.codec().empty()) config_.encoders[target_idx].codec = request.codec();
    if (request.bitrate_bps() > 0) config_.encoders[target_idx].bitrate = request.bitrate_bps();
    if (request.fps() > 0) config_.encoders[target_idx].fps = request.fps();
    if (request.gop() > 0) config_.encoders[target_idx].gop = request.gop();

    HAL_LOG_INFO("CameraDaemon: Reconfigured encoder %s (interrupt: %ums)",
                 stream_name.c_str(), interrupt_ms);

    response.set_success(true);
    response.set_message("Reconfigured successfully");
    response.set_interrupt_ms(interrupt_ms);
    return true;
}
#endif

bool CameraDaemon::set_rtsp_enabled(bool enabled) {
    std::unique_lock<std::shared_mutex> lock(op_mu_);
    if (enabled && !rtsp_server_) {
        // Need to create and start RTSP server
        if (!encoder_mgr_) {
            HAL_LOG_ERROR("CameraDaemon: Cannot enable RTSP without encoder");
            return false;
        }

        rtsp_server_ = std::make_shared<RtspServer>();

        // Register each encoder stream as an RTSP endpoint
        for (auto& ec : config_.encoders) {
            if (!ec.enabled) continue;
            RtspServer::StreamInfo si;
            si.name = ec.stream_name;
            si.codec = ec.codec;
            si.width = ec.width;
            si.height = ec.height;
            si.fps = ec.fps;
            rtsp_server_->add_stream(si);
        }

        // Register audio stream if audio is active
        if (audio_service_ && config_.audio.enabled) {
            RtspServer::StreamInfo asi;
            asi.name = "audio_capture";
            asi.codec = "pcm";
            asi.is_audio = true;
            asi.sample_rate = config_.audio.sample_rate;
            asi.channels = config_.audio.channels;
            rtsp_server_->add_stream(asi);
            // Enable multi-track: video streams carry audio
            rtsp_server_->set_audio_info("audio_capture");
        }

        // Wire keyframe request
        rtsp_server_->set_keyframe_request_cb(
            [this](const std::string& stream_name) {
                if (encoder_mgr_) {
                    HAL_LOG_INFO("RTSP: Requesting keyframe for %s", stream_name.c_str());
                    encoder_mgr_->force_keyframe(stream_name);
                }
            });

        if (!rtsp_server_->start(config_.rtsp_port)) {
            HAL_LOG_ERROR("CameraDaemon: RTSP server failed to start on port %d", config_.rtsp_port);
            rtsp_server_.reset();
            return false;
        }

        // Register with encoded publisher if available
        if (encoded_pub_) {
            auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
            encoded_pub_->add_local_listener(
                [rtsp_weak](const std::string& stream_name, const HalPacketBuffer* packet) {
                    if (auto rtsp = rtsp_weak.lock()) {
                        rtsp->on_packet(stream_name, packet);
                    }
                });
        }

        config_.rtsp_enabled = true;
        HAL_LOG_INFO("CameraDaemon: RTSP server started on port %d", config_.rtsp_port);
        return true;

    } else if (!enabled && rtsp_server_) {
        // Clear local listeners first to prevent callbacks to destroyed RTSP server
        if (encoded_pub_) {
            encoded_pub_->clear_local_listeners();
        }
        // Stop and destroy RTSP server
        rtsp_server_->stop();
        rtsp_server_.reset();
        config_.rtsp_enabled = false;
        HAL_LOG_INFO("CameraDaemon: RTSP server stopped");
        return true;
    }

    // No change needed
    return true;
}

bool CameraDaemon::update_ai_overlay_config(bool enabled, bool draw_labels, bool draw_confidence, uint32_t box_thickness) {
    std::unique_lock<std::shared_mutex> lock(op_mu_);
    if (enabled && !ai_overlay_) {
        if (!hal_loader_ || !hal_loader_->has_draw()) {
            HAL_LOG_ERROR("CameraDaemon: Cannot enable AI overlay without HAL draw ops");
            return false;
        }

        config_.ai_overlay_enabled = true;
        config_.ai_overlay_draw_labels = draw_labels;
        config_.ai_overlay_draw_confidence = draw_confidence;
        config_.ai_overlay_box_thickness = box_thickness;

        return init_ai_overlay();
    }

    if (!enabled && ai_overlay_) {
        ai_overlay_->stop();
        ai_overlay_.reset();
        config_.ai_overlay_enabled = false;
        HAL_LOG_INFO("CameraDaemon: AI overlay disabled");
        return true;
    }

    // Update existing AI overlay config
    if (ai_overlay_) {
        ai_overlay_->update_config(draw_labels, draw_confidence, box_thickness);
        config_.ai_overlay_draw_labels = draw_labels;
        config_.ai_overlay_draw_confidence = draw_confidence;
        config_.ai_overlay_box_thickness = box_thickness;
    }

    return true;
}

#ifdef HAS_GRPC
bool CameraDaemon::update_osd_config(const aipc::camera::OsdConfigRequest& request) {
    std::unique_lock<std::shared_mutex> lock(op_mu_);
    HAL_LOG_INFO("CameraDaemon: update_osd_config: osd_mgr=%p encoder_mgr=%p streams=%d",
                 osd_mgr_.get(), encoder_mgr_.get(), request.streams_size());
    if (!osd_mgr_ || !encoder_mgr_) {
        HAL_LOG_ERROR("CameraDaemon: OSD manager or encoder manager not initialized");
        return false;
    }

    aipc::camera::OsdConfigRequest sanitized;
    sanitize_osd_config_request(request, &sanitized);

    // Cache the full request (incl. disabled overlays) so get_osd_config can
    // echo it back. Disabled overlays are skipped below (never baked) because
    // the Hailo medialib does not reliably honor set_overlay_enabled(false);
    // baking them would let the text bleed through when the eye toggle hides.
    last_osd_request_ = std::make_unique<aipc::camera::OsdConfigRequest>(sanitized);
    // Mirror to disk so the cache survives restart/deploy/OS-upgrade. Best-effort
    // (failures only log); called here so disk == cache == GET before re-baking.
    persist_osd_config_locked(*last_osd_request_);

    // Editor edit-mode: when set, clear-and-reapply bakes NO text overlays so
    // the live stream is clean and the web HTML text proxies are the single
    // layer (no baked/proxy ghosting during drag). Datetime/image still bake
    // (their proxies are frame-only, no doubling). Flipped back on editor exit.
    osd_suppress_bake_ = sanitized.suppress_bake();

    bool any_success = false;

    // Helper: unpack 0xAARRGGBB proto color → HalOsdColor {r, g, b, a}
    auto unpack_color = [](uint32_t argb) -> HalOsdColor {
        return {
            (int)((argb >> 16) & 0xFF),  // r
            (int)((argb >> 8)  & 0xFF),  // g
            (int)( argb        & 0xFF),  // b
            (int)((argb >> 24) & 0xFF),  // a
        };
    };

    for (const auto& stream_cfg : sanitized.streams()) {
        const std::string& stream_name = stream_cfg.stream_name();
        HAL_LOG_INFO("CameraDaemon: OSD processing stream '%s': %d text, %d datetime, %d image overlays",
                     stream_name.c_str(), stream_cfg.text_overlays_size(),
                     stream_cfg.datetime_overlays_size(), stream_cfg.image_overlays_size());

        // Resolve config name → media pipeline name for FROM_MEDIA encoders
        std::string enc_name = stream_name;
        for (const auto& [media_name, config_name] : encoder_name_map_) {
            if (config_name == stream_name) {
                enc_name = media_name;
                break;
            }
        }

        // Get codec context for this stream
        void* codec_ctx = encoder_mgr_->get_codec_ctx(enc_name);
        if (!codec_ctx) {
            HAL_LOG_WARNING("CameraDaemon: No encoder for stream '%s' (resolved '%s'), skipping OSD update",
                           stream_name.c_str(), enc_name.c_str());
            continue;
        }

        // Clear-and-reapply: remove all existing overlays, then add from request.
        // This ensures edits, disables, and deletes take effect.
        int rc = osd_mgr_->clear_overlays(codec_ctx);
        if (rc != 0) {
            HAL_LOG_WARNING("CameraDaemon: clear_overlays failed for stream '%s' (rc=%d), proceeding with add",
                           stream_name.c_str(), rc);
        }

        int overlay_count = 0;

        // Add text overlays. Disabled overlays are skipped (not baked) so a
        // hidden overlay can't bleed through; their config is preserved via
        // last_osd_request_ for the GET round-trip.
        for (const auto& text_overlay : stream_cfg.text_overlays()) {
            // Skip disabled, and skip ALL when the editor is suppressing the bake
            // (edit-mode clean stream). Suppression applies to BOTH text and image
            // overlays (see the image loop below) so their HTML proxies are the
            // single visible layers; datetime is NOT suppressed (no proxy content).
            if (osd_suppress_bake_ || !text_overlay.enabled()) {
                continue;
            }
            HalOsdTextOverlay tc{};
            strncpy(tc.base.id, text_overlay.id().c_str(), sizeof(tc.base.id) - 1);
            if (!text_overlay.text().empty()) {
                strncpy(tc.label, text_overlay.text().c_str(), sizeof(tc.label) - 1);
            }
            tc.base.x = text_overlay.x();
            tc.base.y = text_overlay.y();
            tc.base.enabled = text_overlay.enabled();
            tc.base.z_index = 1;
            tc.base.h_align = static_cast<HalOsdHorizontalAlignment>(text_overlay.h_align());
            tc.base.v_align = static_cast<HalOsdVerticalAlignment>(text_overlay.v_align());
            tc.font_size = static_cast<float>(text_overlay.font_size());
            tc.text_color = unpack_color(text_overlay.text_color());
            strncpy(tc.font_path, "/usr/share/fonts/ttf/LiberationMono-Regular.ttf",
                    sizeof(tc.font_path) - 1);
            rc = osd_mgr_->add_text(codec_ctx, tc);
            if (rc != 0) {
                HAL_LOG_WARNING("CameraDaemon: add_text [%s] failed (rc=%d)", tc.base.id, rc);
            } else {
                overlay_count++;
            }
        }

        // Add datetime overlays (disabled skipped — see text loop note).
        for (const auto& dt_overlay : stream_cfg.datetime_overlays()) {
            if (!dt_overlay.enabled()) {
                continue;
            }
            HalOsdDateTimeOverlay dtc{};
            strncpy(dtc.text.base.id, dt_overlay.id().c_str(), sizeof(dtc.text.base.id) - 1);
            dtc.text.base.x = dt_overlay.x();
            dtc.text.base.y = dt_overlay.y();
            dtc.text.base.enabled = dt_overlay.enabled();
            dtc.text.base.z_index = 1;
            dtc.text.base.h_align = static_cast<HalOsdHorizontalAlignment>(dt_overlay.h_align());
            dtc.text.base.v_align = static_cast<HalOsdVerticalAlignment>(dt_overlay.v_align());
            dtc.text.font_size = static_cast<float>(dt_overlay.font_size());
            if (!dt_overlay.format().empty()) {
                strncpy(dtc.datetime_format, dt_overlay.format().c_str(),
                        sizeof(dtc.datetime_format) - 1);
            }
            dtc.text.text_color = unpack_color(dt_overlay.text_color());
            strncpy(dtc.text.font_path, "/usr/share/fonts/ttf/LiberationMono-Regular.ttf",
                    sizeof(dtc.text.font_path) - 1);
            rc = osd_mgr_->add_datetime(codec_ctx, dtc);
            if (rc != 0) {
                HAL_LOG_WARNING("CameraDaemon: add_datetime [%s] failed (rc=%d)", dtc.text.base.id, rc);
            } else {
                overlay_count++;
            }
        }

        // Add image overlays (disabled skipped — see text loop note).
        for (const auto& img_overlay : stream_cfg.image_overlays()) {
            // Skip disabled, and skip ALL when the editor is suppressing the
            // bake (edit-mode clean stream) so only the HTML image proxy shows
            // the picture and it follows the drag with no baked/proxy doubling
            // or lag. Datetime is intentionally NOT suppressed (it has no proxy
            // content — only corner hotspots — so the baked time stays visible).
            if (osd_suppress_bake_ || !img_overlay.enabled()) {
                continue;
            }
            HalOsdImageOverlay ic{};
            strncpy(ic.base.id, img_overlay.id().c_str(), sizeof(ic.base.id) - 1);
            ic.base.x = img_overlay.x();
            ic.base.y = img_overlay.y();
            ic.base.enabled = img_overlay.enabled();
            ic.base.z_index = 0;
            ic.base.h_align = static_cast<HalOsdHorizontalAlignment>(img_overlay.h_align());
            ic.base.v_align = static_cast<HalOsdVerticalAlignment>(img_overlay.v_align());
            ic.width = img_overlay.width();
            ic.height = img_overlay.height();
            if (!img_overlay.image_path().empty()) {
                strncpy(ic.image_path, img_overlay.image_path().c_str(),
                        sizeof(ic.image_path) - 1);
            }
            rc = osd_mgr_->add_image(codec_ctx, ic);
            if (rc != 0) {
                HAL_LOG_WARNING("CameraDaemon: add_image [%s] failed (rc=%d)", ic.base.id, rc);
            } else {
                overlay_count++;
            }
        }

        osd_enabled_streams_.insert(stream_name);
        HAL_LOG_INFO("CameraDaemon: OSD config applied to stream %s (%d overlays ok)",
                    stream_name.c_str(), overlay_count);
        any_success = true;
    }

    return any_success;
}

bool CameraDaemon::get_osd_config(aipc::camera::OsdConfigResponse& response) {
    std::shared_lock<std::shared_mutex> lock(op_mu_);

    // Echo the last applied request (the source of truth). Disabled overlays
    // are NOT baked (skipped in update_osd_config) so they're absent from the
    // HAL; reading back from HAL would drop them and break the eye-toggle /
    // refresh round-trip. The cache holds every overlay with its full config
    // and enabled flag, exactly as the UI last sent it.
    if (last_osd_request_) {
        for (const auto& s : last_osd_request_->streams()) {
            response.add_streams()->CopyFrom(s);
        }
    }
    return true;
}

// Best-effort disk mirror of the OSD config. Caller MUST hold op_mu_ (only
// update_osd_config calls this, right after caching into last_osd_request_).
// Atomic tmp+rename so a power loss never exposes a half-written file; any IO
// failure only logs — it must NOT alter the in-memory apply result.
void CameraDaemon::persist_osd_config_locked(const aipc::camera::OsdConfigRequest& req) {
    aipc::camera::OsdConfigRequest copy = req;
    copy.set_suppress_bake(false);  // never persist the editor edit-mode transient

    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.always_print_primitive_fields = true;
    std::string json;
    auto st = google::protobuf::util::MessageToJsonString(copy, &json, opts);
    if (!st.ok()) {
        HAL_LOG_ERROR("CameraDaemon: persist OSD: serialize failed: %s",
                      std::string(st.message()).c_str());
        return;
    }

    const std::string tmp = std::string(kOsdConfigPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            HAL_LOG_ERROR("CameraDaemon: persist OSD: open(%s) failed", tmp.c_str());
            return;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            HAL_LOG_ERROR("CameraDaemon: persist OSD: write(%s) failed", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }  // ofstream flushed + closed here

    if (std::rename(tmp.c_str(), kOsdConfigPath) != 0) {
        HAL_LOG_ERROR("CameraDaemon: persist OSD: rename(%s -> %s) failed",
                      tmp.c_str(), kOsdConfigPath);
        ::unlink(tmp.c_str());
        return;
    }
}

// Read the OSD mirror at startup. Returns false on missing file (first boot /
// never configured — INFO, not an error), unparseable JSON (WARNING + Clear — a
// corrupt file never aborts init), or an empty config (no streams to reapply).
// suppress_bake is forced false on the way out so edit-mode can never be restored.
bool CameraDaemon::load_osd_config(aipc::camera::OsdConfigRequest* req) {
    std::ifstream in(kOsdConfigPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted OSD config (%s); starting clean",
                     kOsdConfigPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    req->Clear();
    auto st = google::protobuf::util::JsonStringToMessage(ss.str(), req);
    if (!st.ok()) {
        HAL_LOG_WARNING("CameraDaemon: persisted OSD config unparseable: %s; starting clean",
                        std::string(st.message()).c_str());
        req->Clear();
        return false;
    }
    req->set_suppress_bake(false);

    aipc::camera::OsdConfigRequest sanitized;
    sanitize_osd_config_request(*req, &sanitized);
    req->Swap(&sanitized);

    if (req->streams_size() == 0) {
        return false;  // empty mirror == nothing to reapply
    }
    return true;
}

bool CameraDaemon::reapply_osd_config_after_pipeline_rebuild(const char* reason) {
    if (!osd_mgr_ || !encoder_mgr_) {
        return true;
    }

    std::vector<std::pair<std::string, std::string>> targets;
    aipc::camera::OsdConfigRequest cached;
    bool has_cached = false;
    {
        std::shared_lock<std::shared_mutex> lock(op_mu_);
        targets.reserve(config_.encoders.size());
        for (const auto& enc : config_.encoders) {
            if (!enc.enabled) {
                continue;
            }
            std::string media_name = enc.stream_name;
            for (const auto& [pipeline_name, config_name] : encoder_name_map_) {
                if (config_name == enc.stream_name) {
                    media_name = pipeline_name;
                    break;
                }
            }
            targets.emplace_back(enc.stream_name, media_name);
        }

        if (last_osd_request_) {
            cached.CopyFrom(*last_osd_request_);
            has_cached = cached.streams_size() > 0;
        }
    }

    for (const auto& [display_name, media_name] : targets) {
        void* codec_ctx = encoder_mgr_->get_codec_ctx(media_name);
        if (!codec_ctx) {
            HAL_LOG_WARNING("CameraDaemon: OSD replay after %s: no encoder for '%s' (resolved '%s')",
                            reason ? reason : "pipeline rebuild",
                            display_name.c_str(), media_name.c_str());
            continue;
        }
        int rc = osd_mgr_->clear_overlays(codec_ctx);
        if (rc != 0) {
            HAL_LOG_WARNING("CameraDaemon: OSD replay after %s: clear_overlays failed for '%s' (rc=%d)",
                            reason ? reason : "pipeline rebuild",
                            display_name.c_str(), rc);
        }
    }

    {
        std::unique_lock<std::shared_mutex> lock(op_mu_);
        osd_enabled_streams_.clear();
    }

    if (!has_cached) {
        HAL_LOG_INFO("CameraDaemon: OSD replay after %s: cleared vendor defaults, no cached web OSD",
                     reason ? reason : "pipeline rebuild");
        return true;
    }

    HAL_LOG_INFO("CameraDaemon: OSD replay after %s: reapplying cached web OSD (%d stream(s))",
                 reason ? reason : "pipeline rebuild", cached.streams_size());
    if (!update_osd_config(cached)) {
        HAL_LOG_WARNING("CameraDaemon: OSD replay after %s failed",
                        reason ? reason : "pipeline rebuild");
        return false;
    }
    return true;
}

// Best-effort disk mirror of the privacy-mask/DPM config. Mirrors the OSD helper
// above but is NOT "_locked": set_privacy_mask_config (the sole caller) does NOT
// hold op_mu_ (unlike update_osd_config). No lock is needed anyway — persist only
// serializes its const& argument (no shared-state read) and writes a tmp+rename
// file (atomic; a power loss never exposes a half-written file). Any IO failure
// only logs and never alters the in-memory apply result. dpm_enabled is persisted
// as the *requested* desired state (not the effective start_ok), so a restart
// re-attempts DPM start — desired-state semantics, same as OSD.
void CameraDaemon::persist_privacy_mask_config(const aipc::camera::PrivacyMaskConfig& req) {
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.always_print_primitive_fields = true;
    std::string json;
    auto st = google::protobuf::util::MessageToJsonString(req, &json, opts);
    if (!st.ok()) {
        HAL_LOG_ERROR("CameraDaemon: persist privacy-mask: serialize failed: %s",
                      std::string(st.message()).c_str());
        return;
    }

    const std::string tmp = std::string(kPrivacyMaskConfigPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            HAL_LOG_ERROR("CameraDaemon: persist privacy-mask: open(%s) failed", tmp.c_str());
            return;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            HAL_LOG_ERROR("CameraDaemon: persist privacy-mask: write(%s) failed", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }  // ofstream flushed + closed here

    if (std::rename(tmp.c_str(), kPrivacyMaskConfigPath) != 0) {
        HAL_LOG_ERROR("CameraDaemon: persist privacy-mask: rename(%s -> %s) failed",
                      tmp.c_str(), kPrivacyMaskConfigPath);
        ::unlink(tmp.c_str());
        return;
    }
}

// Read the privacy-mask/DPM mirror at startup. Returns false on missing file
// (first boot / never configured — INFO, not an error), unparseable JSON
// (WARNING + Clear — a corrupt file never aborts init), or an empty config
// (enabled==false AND no regions AND dpm disabled — nothing to reapply). Never
// aborts init; the caller (init, under HAS_GRPC) simply proceeds with defaults.
bool CameraDaemon::load_privacy_mask_config(aipc::camera::PrivacyMaskConfig* req) {
    std::ifstream in(kPrivacyMaskConfigPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted privacy-mask config (%s); starting clean",
                     kPrivacyMaskConfigPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    req->Clear();
    auto st = google::protobuf::util::JsonStringToMessage(ss.str(), req);
    if (!st.ok()) {
        HAL_LOG_WARNING("CameraDaemon: persisted privacy-mask config unparseable: %s; starting clean",
                        std::string(st.message()).c_str());
        req->Clear();
        return false;
    }
    if (!req->enabled() && req->regions_size() == 0 && !req->dpm_enabled()) {
        return false;  // empty mirror == nothing to reapply
    }
    return true;
}

// Persist the transform config (rotation/flip/dewarp/grayscale/dis/eis) to the
// side-file atomically (tmp + rename). Best-effort: a serialize/write/rename
// failure logs but never aborts the (already-applied) HAL apply in the caller.
void CameraDaemon::persist_transform_config(const aipc::camera::TransformConfig& req) {
    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.always_print_primitive_fields = true;
    std::string json;
    auto st = google::protobuf::util::MessageToJsonString(req, &json, opts);
    if (!st.ok()) {
        HAL_LOG_ERROR("CameraDaemon: persist transform: serialize failed: %s",
                      std::string(st.message()).c_str());
        return;
    }

    const std::string tmp = std::string(kTransformConfigPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            HAL_LOG_ERROR("CameraDaemon: persist transform: open(%s) failed", tmp.c_str());
            return;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            HAL_LOG_ERROR("CameraDaemon: persist transform: write(%s) failed", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }  // ofstream flushed + closed here

    if (std::rename(tmp.c_str(), kTransformConfigPath) != 0) {
        HAL_LOG_ERROR("CameraDaemon: persist transform: rename(%s -> %s) failed",
                      tmp.c_str(), kTransformConfigPath);
        ::unlink(tmp.c_str());
        return;
    }
}

// Read the transform mirror at startup. Returns false on missing file (first
// boot / never configured — INFO, not an error), unparseable JSON (WARNING +
// Clear — a corrupt file never aborts init), or an identity config (rotation==0
// && flip==0 && !dewarp && !grayscale && !dis && !eis — nothing to reapply, and
// re-pushing identity would issue a needless dynamic_change_image_config call).
// Never aborts init; the caller (init, under HAS_GRPC) proceeds with YAML
// defaults.
bool CameraDaemon::load_transform_config(aipc::camera::TransformConfig* req) {
    std::ifstream in(kTransformConfigPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted transform config (%s); starting clean",
                     kTransformConfigPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    req->Clear();
    auto st = google::protobuf::util::JsonStringToMessage(ss.str(), req);
    if (!st.ok()) {
        HAL_LOG_WARNING("CameraDaemon: persisted transform config unparseable: %s; starting clean",
                        std::string(st.message()).c_str());
        req->Clear();
        return false;
    }
    if (req->rotation() == 0 && req->flip() == 0 &&
        !req->dewarp() && !req->grayscale() && !req->dis() && !req->eis()) {
        return false;  // identity == nothing to reapply
    }
    return true;
}

// Persist ONE scalar config field into the mirror (load-merge-write so each
// field is independent — never clobbers the others). The mirror owns only
// allow-listed scalar profile fields. Atomic tmp+rename, best-effort (a
// serialize/write/rename failure logs but never aborts the already-applied HAL
// write in the caller). Same shape as persist_transform_config above.
void CameraDaemon::persist_config_field(const std::string& field_path,
                                        aipc::camera::ConfigFieldType type,
                                        const std::string& value) {
    aipc::camera::MediaConfigFields fields;
    load_config_fields(&fields);  // best-effort: miss/corrupt -> empty map

    auto& fv = (*fields.mutable_fields())[field_path];
    fv.set_type(type);
    fv.set_value(value);

    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.always_print_primitive_fields = true;
    std::string json;
    auto st = google::protobuf::util::MessageToJsonString(fields, &json, opts);
    if (!st.ok()) {
        HAL_LOG_ERROR("CameraDaemon: persist config-field: serialize failed: %s",
                      std::string(st.message()).c_str());
        return;
    }

    const std::string tmp = std::string(kMediaConfigFieldsPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            HAL_LOG_ERROR("CameraDaemon: persist config-field: open(%s) failed", tmp.c_str());
            return;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            HAL_LOG_ERROR("CameraDaemon: persist config-field: write(%s) failed", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }  // ofstream flushed + closed here

    if (std::rename(tmp.c_str(), kMediaConfigFieldsPath) != 0) {
        HAL_LOG_ERROR("CameraDaemon: persist config-field: rename(%s -> %s) failed",
                      tmp.c_str(), kMediaConfigFieldsPath);
        ::unlink(tmp.c_str());
        return;
    }
}

// Read the config-field mirror at startup. Returns false on missing file (first
// boot / never configured — INFO, not an error), unparseable JSON (WARNING +
// Clear — a corrupt file never aborts init), or an empty map (nothing to
// reapply). Never aborts init; the caller (init, under HAS_GRPC) proceeds with
// the HAL profile defaults.
bool CameraDaemon::load_config_fields(aipc::camera::MediaConfigFields* req) {
    std::ifstream in(kMediaConfigFieldsPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted config-field mirror (%s); starting clean",
                     kMediaConfigFieldsPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    req->Clear();
    auto st = google::protobuf::util::JsonStringToMessage(ss.str(), req);
    if (!st.ok()) {
        HAL_LOG_WARNING("CameraDaemon: persisted config-field mirror unparseable: %s; starting clean",
                        std::string(st.message()).c_str());
        req->Clear();
        return false;
    }
    return req->fields_size() > 0;  // empty mirror == nothing to reapply
}

#ifdef HAS_GRPC
// set_config_field: write one allow-listed scalar profile field.
//
// Allow-list gate first (prevents the two-writer race with typed RPCs that own
// dewarp/bitrate/gop/rotation/flip/ISP), then the HAL applies the override via
// apply_profile_override_and_refresh (no full teardown — lighter than the
// rotation reinit path in set_transform_config). op_mu_ serializes against the
// data path exactly like update_encoder_config / update_osd_config. On success
// the value is mirrored to /data/aipc/etc so the next boot's replay restores it
// (replay-on-boot wins over HAL profile persistence).
//
// ConfigFieldType and HalConfigFieldType are defined 1:1 (BOOL=0..STRING=4);
// static_cast is exact. The HAL converts the string value per field_type.
bool CameraDaemon::set_config_field(const aipc::camera::SetConfigFieldRequest& req,
                                    std::string* msg) {
    if (!is_config_field_allowed(req.field_path())) {
        if (msg) *msg = "field not in allow-list: " + req.field_path();
        HAL_LOG_WARNING("CameraDaemon: set_config_field rejected (not allow-listed): %s",
                        req.field_path().c_str());
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_ || !media_ops->set_config_field) {
        if (msg) *msg = "media ops not available";
        return false;
    }

    const HalConfigFieldType hal_type = static_cast<HalConfigFieldType>(req.type());
    int ret = media_ops->set_config_field(media_ctx_, req.field_path().c_str(),
                                          hal_type, req.value().c_str());
    if (ret < 0) {
        if (msg) *msg = "HAL set_config_field failed: " + std::to_string(ret);
        return false;
    }

    persist_config_field(req.field_path(), req.type(), req.value());
    HAL_LOG_INFO("CameraDaemon: set_config_field %s = %s (type=%d)",
                 req.field_path().c_str(), req.value().c_str(), (int)req.type());
    return true;
}

// get_config_field: read one scalar profile field straight from the HAL (sees
// the current runtime-overridden value). Shared op_mu_ like every getter.
bool CameraDaemon::get_config_field(const std::string& field_path,
                                    aipc::camera::ConfigFieldType& type,
                                    std::string& value, std::string* msg) {
    std::shared_lock<std::shared_mutex> lock(op_mu_);
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_ || !media_ops->get_config_field) {
        if (msg) *msg = "media ops not available";
        return false;
    }

    HalConfigFieldType hal_type = HAL_CONFIG_FIELD_BOOL;  // out-param, overwritten by HAL
    const char* cstr = nullptr;
    int ret = media_ops->get_config_field(media_ctx_, field_path.c_str(), &hal_type, &cstr);
    if (ret < 0 || cstr == nullptr) {
        if (msg) *msg = "HAL get_config_field failed: " + std::to_string(ret);
        return false;
    }
    type = static_cast<aipc::camera::ConfigFieldType>(static_cast<int>(hal_type));
    value = cstr;
    return true;
}
#else
// Stubs for the no-gRPC build: the unguarded header decls (see camera_daemon.h)
// always exist, so provide trivially-failing definitions when camera.pb.h /
// HAL_MEDIA_OPS aren't pulled in. Real callers (camera_control_service) only
// compile under HAS_GRPC, so these are never invoked in practice.
bool CameraDaemon::set_config_field(const aipc::camera::SetConfigFieldRequest&, std::string* msg) {
    if (msg) *msg = "built without gRPC/HAL config-field support";
    return false;
}
bool CameraDaemon::get_config_field(const std::string&, aipc::camera::ConfigFieldType&,
                                    std::string&, std::string* msg) {
    if (msg) *msg = "built without gRPC/HAL config-field support";
    return false;
}
#endif

// Fill an ISPUpdateRequest from the cached ISP state. Mirrors the field mapping
// in get_isp_config so the persisted snapshot is exactly what the UI last saw.
// All fields are populated (set), so a replayed request fires every branch of
// update_isp_settings and re-pushes the complete state, not a delta.
void CameraDaemon::build_isp_request_from_cache(aipc::camera::ISPUpdateRequest* req) const {
    const HalIspImageConfig& ic = cached_isp_state_;
    req->set_manual_mode(ic.manual_config.manual_state);
    req->set_brightness(ic.manual_config.brightness);
    req->set_contrast(ic.manual_config.contrast);
    req->set_saturation(ic.manual_config.saturation);
    req->set_sharpness(ic.manual_config.sharpness);
    req->set_auto_exposure(ic.exposure_config.auto_exposure);
    req->set_backlight(ic.exposure_config.backlight);
    req->set_exposure_time_us(ic.exposure_config.exposure_time_us);
    req->set_gain(ic.exposure_config.gain);
    req->set_noise_reduction(ic.noise_reduction);
    req->set_wdr_value(ic.wdr_value);
    req->set_powerline_freq(static_cast<int32_t>(ic.pwr_freq));
    req->set_awb_index(ic.awb_idx);
}

// Persist the full ISP snapshot (cached_isp_state_) atomically (tmp + rename).
// Best-effort: a failure logs but never alters the (already-applied) update
// result. Called from update_isp_settings after a successful HAL write.
void CameraDaemon::persist_isp_config() {
    aipc::camera::ISPUpdateRequest req;
    build_isp_request_from_cache(&req);

    google::protobuf::util::JsonPrintOptions opts;
    opts.add_whitespace = true;
    opts.always_print_primitive_fields = true;
    std::string json;
    auto st = google::protobuf::util::MessageToJsonString(req, &json, opts);
    if (!st.ok()) {
        HAL_LOG_ERROR("CameraDaemon: persist isp: serialize failed: %s",
                      std::string(st.message()).c_str());
        return;
    }

    const std::string tmp = std::string(kIspConfigPath) + ".tmp";
    {
        std::ofstream out(tmp, std::ios::out | std::ios::trunc);
        if (!out.is_open()) {
            HAL_LOG_ERROR("CameraDaemon: persist isp: open(%s) failed", tmp.c_str());
            return;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            HAL_LOG_ERROR("CameraDaemon: persist isp: write(%s) failed", tmp.c_str());
            ::unlink(tmp.c_str());
            return;
        }
    }  // ofstream flushed + closed here

    if (std::rename(tmp.c_str(), kIspConfigPath) != 0) {
        HAL_LOG_ERROR("CameraDaemon: persist isp: rename(%s -> %s) failed",
                      tmp.c_str(), kIspConfigPath);
        ::unlink(tmp.c_str());
        return;
    }
}

// Read the ISP snapshot at startup. Returns false on missing file (first boot /
// never configured — INFO, not an error) or unparseable JSON (WARNING + Clear —
// a corrupt file never aborts init). Never aborts init; the caller (init, under
// HAS_GRPC) proceeds with the HAL/cached defaults.
bool CameraDaemon::load_isp_config(aipc::camera::ISPUpdateRequest* req) {
    std::ifstream in(kIspConfigPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted ISP config (%s); starting clean",
                     kIspConfigPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();

    req->Clear();
    auto st = google::protobuf::util::JsonStringToMessage(ss.str(), req);
    if (!st.ok()) {
        HAL_LOG_WARNING("CameraDaemon: persisted ISP config unparseable: %s; starting clean",
                        std::string(st.message()).c_str());
        req->Clear();
        return false;
    }
    return true;
}

#endif

// --- Active-profile persistence (unconditional; plain-string JSON, no proto) ---
namespace {
// Minimal JSON string escaper for a single value. Profile names are simple
// identifiers (e.g. "FULL_PERFORMANCE", "LOW_LIGHT") but escape rigorously
// anyway so a malformed name can never break the mirror file.
std::string json_escape_string(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}
}  // namespace

void CameraDaemon::persist_profile_config(const std::string& profile_name) {
    std::string content = "{\"profile_name\":\"" + json_escape_string(profile_name) + "\"}\n";
    const std::string tmp = std::string(kProfileConfigPath) + ".tmp";
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        HAL_LOG_WARNING("CameraDaemon: failed to open profile mirror for write: %s", tmp.c_str());
        return;
    }
    out << content;
    out.close();
    if (!out) {
        HAL_LOG_WARNING("CameraDaemon: failed to write profile mirror: %s", tmp.c_str());
        return;
    }
    if (std::rename(tmp.c_str(), kProfileConfigPath) != 0) {
        HAL_LOG_WARNING("CameraDaemon: failed to rename profile mirror %s -> %s",
                        tmp.c_str(), kProfileConfigPath);
        std::remove(tmp.c_str());
    }
}

bool CameraDaemon::load_profile_config(std::string* profile_name) {
    std::ifstream in(kProfileConfigPath);
    if (!in.is_open()) {
        HAL_LOG_INFO("CameraDaemon: no persisted profile (%s); starting from default",
                     kProfileConfigPath);
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    in.close();
    const std::string text = ss.str();

    // Minimal tolerant parse: locate "profile_name", then the next quoted string.
    const std::string key = "\"profile_name\"";
    std::size_t k = text.find(key);
    if (k == std::string::npos) {
        HAL_LOG_WARNING("CameraDaemon: persisted profile missing profile_name key; starting from default");
        return false;
    }
    std::size_t i = k + key.size();
    // Skip whitespace and the ':' between key and value.
    while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' ||
           text[i] == '\r' || text[i] == ':')) {
        ++i;
    }
    if (i >= text.size() || text[i] != '"') {
        HAL_LOG_WARNING("CameraDaemon: persisted profile value not a string; starting from default");
        return false;
    }
    ++i;  // opening quote
    std::string value;
    bool escape = false;
    for (; i < text.size(); ++i) {
        char c = text[i];
        if (escape) {
            switch (c) {
                case '"':  value += '"'; break;
                case '\\': value += '\\'; break;
                case 'n':  value += '\n'; break;
                case 'r':  value += '\r'; break;
                case 't':  value += '\t'; break;
                case 'b':  value += '\b'; break;
                case 'f':  value += '\f'; break;
                default:   value += c; break;  // tolerate unknown escapes
            }
            escape = false;
        } else if (c == '\\') {
            escape = true;
        } else if (c == '"') {
            *profile_name = value;
            return true;
        } else {
            value += c;
        }
    }
    HAL_LOG_WARNING("CameraDaemon: persisted profile string unterminated; starting from default");
    return false;
}

#ifdef HAS_GRPC
void CameraDaemon::start_grpc_server() {
    std::string server_address("unix:///run/aipc/camera-control.sock");

    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());

    // Optional: LensHAL service (exposes lens motor control over gRPC)
    if (!config_.lens_bridge_lib.empty()) {
        LensHalConfig lens_cfg;
        lens_cfg.library_path    = config_.lens_bridge_lib;
        lens_cfg.serial_device   = config_.lens_serial_device;
        lens_cfg.baud_rate       = config_.lens_baud_rate;
        lens_cfg.timeout_ms      = config_.lens_timeout_ms;
        lens_cfg.zoom_min        = config_.lens_zoom_min;
        lens_cfg.zoom_max        = config_.lens_zoom_max;
        lens_cfg.focus_min       = config_.lens_focus_min;
        lens_cfg.focus_max       = config_.lens_focus_max;
        auto lens_bundle = CreateLensHalService(lens_cfg);
        lens_controller_ = lens_bundle.controller;
        lens_hal_service_ = std::move(lens_bundle.service);
        if (lens_hal_service_) {
            builder.RegisterService(lens_hal_service_.get());
            HAL_LOG_INFO("CameraDaemon: LensHAL service registered (bridge=%s)", config_.lens_bridge_lib.c_str());
        }
    }

    if (config_.autofocus.enabled && lens_controller_ && hal_loader_ &&
        hal_loader_->has_isp() && video_source_ && frame_router_) {
        autofocus_controller_ = std::make_unique<AutofocusController>(
            hal_loader_->isp(), hal_loader_->video(), video_source_->video_ctx(),
            frame_router_.get(), lens_controller_, config_.autofocus, 0, 0);
    } else if (config_.autofocus.enabled) {
        HAL_LOG_WARNING("CameraDaemon: autofocus unavailable (lens/ISP/video missing)");
    }

    camera_control_service_ = std::make_unique<CameraControlServiceImpl>(this);
    builder.RegisterService(camera_control_service_.get());

    grpc_server_ = builder.BuildAndStart();
    HAL_LOG_INFO("CameraDaemon: gRPC CameraControl listening on %s", server_address.c_str());

    // Fix socket permissions for container access (GID 1001 = aipc group)
    const char* sock_path = "/run/aipc/camera-control.sock";
    chmod(sock_path, 0660);
    chown(sock_path, -1, 1001);

    if (autofocus_controller_) autofocus_controller_->start();
}

void CameraDaemon::stop_grpc_server() {
    if (autofocus_controller_) {
        autofocus_controller_->stop();
        autofocus_controller_.reset();
    }
    if (grpc_server_) {
        grpc_server_->Shutdown();
        grpc_server_.reset();
    }
    lens_controller_ = nullptr;
    lens_hal_service_.reset();
    camera_control_service_.reset();
}
#endif

/* ========== Private init methods ========== */

bool CameraDaemon::init_hal() {
    hal_loader_ = std::make_unique<HalLoader>();
    // overlay_lib: HAL_DRAW_OPS from draw HAL .so
    // v2 monolithic: video/codec/overlay are all in the same libaipc_hal.so
    std::string overlay_lib = config_.ai_overlay_lib;
    if (overlay_lib.empty() && config_.ai_overlay_enabled) {
        // v2: use the same library as codec (monolithic libaipc_hal.so)
        if (!config_.codec_lib.empty()) {
            overlay_lib = config_.codec_lib;
        } else if (!config_.video_lib.empty()) {
            overlay_lib = config_.video_lib;
        }
    }
    if (!hal_loader_->load(config_.video_lib, config_.codec_lib, overlay_lib, config_.led_lib, config_.audio.audio_lib)) {
        HAL_LOG_ERROR("CameraDaemon: Failed to load HAL libraries");
        return false;
    }
    if (hal_loader_->has_osd()) {
        HAL_LOG_INFO("CameraDaemon: HAL OSD ops loaded");
    }
    if (hal_loader_->has_draw()) {
        HAL_LOG_INFO("CameraDaemon: HAL draw ops loaded");
    }
    if (hal_loader_->has_led()) {
        HAL_LOG_INFO("CameraDaemon: HAL LED/IR-cut ops loaded");
    }
    if (hal_loader_->has_audio()) {
        HAL_LOG_INFO("CameraDaemon: HAL audio ops loaded");
    }
    return true;
}

bool CameraDaemon::init_media() {
    auto* media_ops = hal_loader_->media();
    if (!media_ops) {
        HAL_LOG_INFO("CameraDaemon: HAL_MEDIA_OPS not available, using standalone mode");
        return true;  // Non-media platforms can work without media pipeline
    }

    if (config_.media_config_path.empty() && config_.media_config_json.empty()) {
        // No explicit media config_path/json supplied by the platform. Fall through to
        // media_ops->init() so the HAL uses its compiled-in default medialib config
        // (hailo15_media_impl.cpp materialize_default_medialib_config) instead of
        // skipping the pipeline. The platform layer intentionally no longer maintains a
        // module-specific config_path; the baked-in default is the source of truth.
        HAL_LOG_INFO("CameraDaemon: No media config_path provided; using HAL compiled-in "
                     "default medialib config");
    }

    HAL_LOG_INFO("CameraDaemon: Initializing media pipeline (config_path=%s)",
                 config_.media_config_path.c_str());

    // Verify config file exists before passing to HAL
    if (!config_.media_config_path.empty()) {
        struct stat st;
        if (stat(config_.media_config_path.c_str(), &st) != 0) {
            HAL_LOG_ERROR("CameraDaemon: Media config file not found: %s (errno=%d: %s)",
                         config_.media_config_path.c_str(), errno, strerror(errno));
            HAL_LOG_ERROR("CameraDaemon: Expected location: /usr/bin/medialib_config.json "
                         "or /opt/aipc/etc/media_library.json");
            return false;
        }
    }

    HalMediaConfig mcfg{};
    mcfg.config_path = config_.media_config_path.c_str();
    mcfg.config_json = config_.media_config_json.empty()
                       ? nullptr : config_.media_config_json.c_str();
    mcfg.backup_folder_path = config_.backup_folder_path.empty()
                              ? nullptr : config_.backup_folder_path.c_str();

    // Serialize YAML encoder settings as explicit overrides for HAL.
    // This replaces auto-detection from vendor medialib application_settings,
    // ensuring ALL profiles get corrected encoder dimensions.
    {
        static const std::vector<std::pair<std::string, std::string>> name_map = {
            {"main", "sink0"}, {"sub", "sink1"}, {"third", "sink2"}
        };
        std::ostringstream oss;
        oss << "[";
        bool first = true;
        for (const auto& [sname, sid] : name_map)
        {
            for (const auto& ec : config_.encoders)
            {
                if (ec.stream_name == sname && ec.enabled)
                {
                    if (!first) oss << ",";
                    oss << "{\"stream_id\":\"" << sid << "\","
                        << "\"width\":" << ec.width << ","
                        << "\"height\":" << ec.height << ","
                        << "\"framerate\":" << ec.fps << ","
                        << "\"codec\":\"" << ec.codec << "\","
                        << "\"bitrate\":" << ec.bitrate << ","
                        << "\"gop\":" << ec.gop << "}";
                    first = false;
                    break;
                }
            }
        }
        oss << "]";
        static std::string encoder_overrides_storage;
        encoder_overrides_storage = oss.str();
        mcfg.encoder_overrides_json = encoder_overrides_storage.c_str();
        HAL_LOG_INFO("CameraDaemon: Encoder overrides JSON: %s",
                     encoder_overrides_storage.c_str());
    }

    int ret = media_ops->init(&mcfg, &media_ctx_);
    if (ret < 0 || !media_ctx_) {
        HAL_LOG_ERROR("CameraDaemon: Media pipeline init failed: %d (config_path=%s)",
                     ret, config_.media_config_path.c_str());
        return false;
    }

    // Prefer auto-feed for stability and lower copy pressure.
    // If unavailable/failed, automatically fallback to manual feed mode.
    encoder_auto_feed_enabled_.store(false);
    if (media_ops->set_encoder_auto_feed) {
        ret = media_ops->set_encoder_auto_feed(media_ctx_, true);
        if (ret < 0) {
            HAL_LOG_WARNING("CameraDaemon: set_encoder_auto_feed(true) failed: %d, fallback to manual feed", ret);
            if (media_ops->set_encoder_auto_feed(media_ctx_, false) < 0) {
                HAL_LOG_WARNING("CameraDaemon: set_encoder_auto_feed(false) also failed");
            }
        } else {
            encoder_auto_feed_enabled_.store(true);
            HAL_LOG_INFO("CameraDaemon: Encoder auto-feed enabled");
        }
    } else {
        HAL_LOG_WARNING("CameraDaemon: HAL has no set_encoder_auto_feed; using manual feed mode");
    }

    HAL_LOG_INFO("CameraDaemon: Media pipeline initialized, ctx=%p", media_ctx_);
    return true;
}

bool CameraDaemon::init_video() {
    if (!hal_loader_->has_video()) return false;

    video_source_ = std::make_unique<VideoSource>(hal_loader_->video());

    auto* media_ops = hal_loader_->media();
    if (media_ops && media_ctx_) {
        // v2 media pipeline mode: get pre-created video contexts
        void* video_list = nullptr;
        uint32_t video_count = 0;
        int ret = media_ops->get_video_list(media_ctx_, &video_list, &video_count);
        if (ret < 0 || !video_list || video_count == 0) {
            HAL_LOG_ERROR("CameraDaemon: get_video_list failed: ret=%d count=%u",
                         ret, video_count);
            return false;
        }

        // Cast to pointer array
        void** vlist = static_cast<void**>(video_list);
        HAL_LOG_INFO("CameraDaemon: Media pipeline provided %u video contexts", video_count);

        if (!video_source_->init_from_context(vlist, video_count)) {
            HAL_LOG_ERROR("CameraDaemon: VideoSource init_from_context failed");
            return false;
        }

        // Build stream name mapping and override config with pipeline actual values
        auto& vs_streams = video_source_->streams();
        for (size_t i = 0; i < vs_streams.size() && i < config_.streams.size(); i++) {
            video_name_map_[vs_streams[i].name] = config_.streams[i].name;

            // Override YAML stream params with pipeline actual values
            auto* vc = static_cast<HalVideoContext*>(vlist[i]);
            config_.streams[i].width = vc->config.width;
            config_.streams[i].height = vc->config.height;
            config_.streams[i].fps = vc->config.framerate;
            HAL_LOG_INFO("CameraDaemon: Auto-discovered video stream '%s' → '%s' (%ux%u@%u)",
                        vs_streams[i].name.c_str(), config_.streams[i].name.c_str(),
                        vc->config.width, vc->config.height, vc->config.framerate);
        }

        // Extra pipeline streams beyond YAML config — append with pipeline name
        for (size_t i = config_.streams.size(); i < vs_streams.size(); i++) {
            auto* vc = static_cast<HalVideoContext*>(vlist[i]);
            StreamCfg s;
            s.name = vs_streams[i].name;
            s.width = vc->config.width;
            s.height = vc->config.height;
            s.fps = vc->config.framerate;
            s.pool_max_buffers = 8;
            s.max_queue_size = 12;
            config_.streams.push_back(s);
            HAL_LOG_WARNING("CameraDaemon: Extra pipeline stream '%s' (%ux%u@%u, no config name mapping)",
                           s.name.c_str(), s.width, s.height, s.fps);
        }
    } else {
        // Legacy CSI mode: build HalVideoConfig and init directly
        HalVideoConfig vcfg{};
        vcfg.type = HAL_VIDEO_TYPE_CSI;
        vcfg.path = const_cast<char*>(config_.device_path.c_str());
        vcfg.width = config_.device_width;
        vcfg.height = config_.device_height;
        vcfg.framerate = config_.device_fps;
        vcfg.format = static_cast<HalPixelFormat>(config_.device_format);

        if (!video_source_->init(vcfg)) {
            HAL_LOG_ERROR("CameraDaemon: VideoSource init failed");
            return false;
        }

        // Register streams from config (CSI mode)
        for (auto& s : config_.streams) {
            video_source_->register_stream(s.name);
        }
    }

    return true;
}

bool CameraDaemon::init_encoders() {
    if (!hal_loader_->has_codec()) {
        HAL_LOG_INFO("CameraDaemon: Codec HAL not available, encoding disabled");
        encoder_mgr_ = nullptr;
        osd_mgr_ = nullptr;
        return true;  // Optional
    }

    encoder_mgr_ = std::make_unique<EncoderManager>(hal_loader_->codec());

    // OSD uses separate HalOsdOps loaded from codec lib
    if (hal_loader_->has_osd()) {
        osd_mgr_ = std::make_unique<OsdManager>(hal_loader_->osd());
    } else {
        HAL_LOG_WARNING("CameraDaemon: HAL OSD ops not available, OSD disabled");
    }

    // Encoder callback -> only enqueue to EncodedPublisher
    // Note: in FROM_MEDIA mode, stream_name is the media pipeline ID (e.g. "sink0")
    // We translate it to the config name (e.g. "main") via encoder_name_map_
    encoder_mgr_->set_output_callback(
        [this](const std::string& stream_name, const HalPacketBuffer* packet) {
            std::string name = stream_name;
            {
                std::shared_lock<std::shared_mutex> lock(op_mu_);
                auto it = encoder_name_map_.find(stream_name);
                if (it != encoder_name_map_.end()) name = it->second;
            }

            if (encoded_pub_) {
                encoded_pub_->on_packet(name, packet);
            }
        });

    // Save YAML-desired encoder params before pipeline auto-discovery overwrites them.
    // Used after init to detect if user had previously changed params via ReconfigureEncoder.
    struct YamlDesired {
        uint32_t width, height, fps, bitrate, gop;
        std::string codec;
    };
    std::unordered_map<std::string, YamlDesired> yaml_desired;
    for (const auto& ec : config_.encoders) {
        yaml_desired[ec.stream_name] = {ec.width, ec.height, ec.fps, ec.bitrate, ec.gop, ec.codec};
    }

    auto* media_ops = hal_loader_->media();
    if (media_ops && media_ctx_) {
        // v2 media pipeline mode: get pre-created codec contexts
        void* codec_list = nullptr;
        uint32_t codec_count = 0;
        int ret = media_ops->get_codec_list(media_ctx_, &codec_list, &codec_count);
        if (ret < 0 || !codec_list || codec_count == 0) {
            HAL_LOG_ERROR("CameraDaemon: get_codec_list failed: ret=%d count=%u",
                         ret, codec_count);
            return false;
        }

        void** clist = static_cast<void**>(codec_list);
        HAL_LOG_INFO("CameraDaemon: Media pipeline provided %u codec contexts", codec_count);

        for (uint32_t i = 0; i < codec_count; i++) {
            auto* cc = static_cast<HalCodecContext*>(clist[i]);
            std::string name(cc->codec_name);
            if (name.empty()) {
                HAL_LOG_WARNING("CameraDaemon: codec context[%u] has empty name, skipping", i);
                continue;
            }
            if (!encoder_mgr_->create_from_context(name, clist[i])) {
                HAL_LOG_ERROR("CameraDaemon: Failed to create FROM_MEDIA encoder for %s",
                             name.c_str());
            }
        }

        // Build encoder name mapping and override config with pipeline actual values.
        // Match by position among enabled encoders only (disabled encoders are excluded
        // from the override JSON, so pipeline only creates codecs for enabled ones).
        uint32_t enc_idx = 0;
        for (uint32_t i = 0; i < codec_count && enc_idx < config_.encoders.size(); ) {
            // Skip disabled encoders in YAML
            while (enc_idx < config_.encoders.size() && !config_.encoders[enc_idx].enabled)
                enc_idx++;
            if (enc_idx >= config_.encoders.size()) break;

            auto* cc = static_cast<HalCodecContext*>(clist[i]);
            std::string media_name(cc->codec_name);
            if (media_name.empty()) { i++; continue; }
            encoder_name_map_[media_name] = config_.encoders[enc_idx].stream_name;

            // Override YAML encoder params with pipeline actual values
            auto& ec = config_.encoders[enc_idx];
            ec.width = cc->config.width;
            ec.height = cc->config.height;
            ec.fps = cc->config.framerate;
            ec.bitrate = cc->config.bitrate;
            ec.gop = cc->config.intra_pic_rate;
            ec.codec = (cc->config.packet_type == HAL_PACKET_TYPE_H265) ? "h265" : "h264";
            HAL_LOG_INFO("CameraDaemon: Auto-discovered encoder '%s' → '%s' (%ux%u %s %ubps)",
                        media_name.c_str(), ec.stream_name.c_str(),
                        ec.width, ec.height, ec.codec.c_str(), ec.bitrate);
            enc_idx++;
            i++;
        }

        // Extra encoders beyond YAML config — append with pipeline name
        for (uint32_t i = 0; i < codec_count; i++) {
            auto* cc = static_cast<HalCodecContext*>(clist[i]);
            std::string media_name(cc->codec_name);
            if (media_name.empty()) continue;
            // Skip if already mapped above
            if (encoder_name_map_.count(media_name)) continue;

            EncoderCfg ec;
            ec.stream_name = media_name;
            ec.codec = (cc->config.packet_type == HAL_PACKET_TYPE_H265) ? "h265" : "h264";
            ec.width = cc->config.width;
            ec.height = cc->config.height;
            ec.fps = cc->config.framerate;
            ec.bitrate = cc->config.bitrate;
            ec.gop = cc->config.intra_pic_rate;
            config_.encoders.push_back(ec);
            HAL_LOG_WARNING("CameraDaemon: Extra encoder '%s' (%ux%u %s, no config name mapping)",
                           ec.stream_name.c_str(), ec.width, ec.height, ec.codec.c_str());
        }
    } else {
        // Legacy mode: manually init encoders from config
        for (auto& ec : config_.encoders) {
            HalCodecConfig codec_cfg{};
            codec_cfg.type = HAL_CODEC_TYPE_HW;
            if (ec.codec == "h265") {
                codec_cfg.packet_type = HAL_PACKET_TYPE_H265;
            } else {
                codec_cfg.packet_type = HAL_PACKET_TYPE_H264;
            }
            codec_cfg.path = nullptr;
            codec_cfg.width = ec.width;
            codec_cfg.height = ec.height;
            codec_cfg.format = HAL_PIX_FMT_NV12;
            codec_cfg.framerate = ec.fps;
            codec_cfg.bitrate = ec.bitrate;
            codec_cfg.gop_size = ec.gop;
            codec_cfg.rc_mode = ec.cbr ? HAL_RC_CBR : HAL_RC_VBR;
            if (!ec.rc_mode.empty()) {
                if (ec.rc_mode == "cbr")       codec_cfg.rc_mode = HAL_RC_CBR;
                else if (ec.rc_mode == "vbr")  codec_cfg.rc_mode = HAL_RC_VBR;
                else if (ec.rc_mode == "cvbr") codec_cfg.rc_mode = HAL_RC_CVBR;
                else if (ec.rc_mode == "cqp")  codec_cfg.rc_mode = HAL_RC_CQP;
            }
            codec_cfg.qp_min = ec.qp_min;
            codec_cfg.qp_max = ec.qp_max;

            if (!encoder_mgr_->create(ec.stream_name, codec_cfg)) {
                HAL_LOG_ERROR("CameraDaemon: Failed to create encoder for %s",
                             ec.stream_name.c_str());
            }
        }
    }

    // Configure OSD overlays on encoder handles
    configure_osd();

    // Apply YAML-desired overrides on top of pipeline defaults (FROM_MEDIA mode).
    // Bitrate and GOP overrides are applied at startup via override_stream_params.
    // HAL only modifies intra_pic_rate (keyframe interval), keeping gop_size intact.
    //
    // Dimension/fps/codec changes require patching the medialib encoder JSON config
    // files on disk before init, because:
    //   1) override_stream_params with dimension changes while pipeline is running
    //      causes SIGSEGV in the Hantro VC8000E encoder
    //   2) reconfigure_pipeline fails because the medialib validates content_hash
    //      and rejects dynamically generated configs
    if (media_ops && media_ctx_ && media_ops->override_stream_params) {
        for (const auto& ec : config_.encoders) {
            auto it = yaml_desired.find(ec.stream_name);
            if (it == yaml_desired.end()) continue;
            const auto& yd = it->second;

            bool bitrate_diff = (yd.bitrate != 0 && yd.bitrate != ec.bitrate);
            bool gop_diff = (yd.gop != 0 && yd.gop != ec.gop);
            bool width_diff = (yd.width != 0 && yd.width != ec.width);
            bool height_diff = (yd.height != 0 && yd.height != ec.height);
            bool fps_diff = (yd.fps != 0 && yd.fps != ec.fps);

            // Log dimension/fps mismatch (requires medialib config patch)
            if (width_diff || height_diff || fps_diff) {
                HAL_LOG_WARNING("CameraDaemon: Dimension/fps mismatch for %s "
                                "(pipeline=%ux%u fps=%u, yaml=%ux%u fps=%u) — "
                                "patch medialib encoder JSON to match",
                                ec.stream_name.c_str(), ec.width, ec.height, ec.fps,
                                yd.width, yd.height, yd.fps);
            }

            // Only apply bitrate override at startup.
            // GOP override via set_override_parameters() before pipeline start
            // causes medialib encoder init failure ("Failed to init gop config").
            // GOP will be applied via EncoderManager::set_gop() after pipeline starts.
            if (!bitrate_diff) continue;

            std::string pipeline_id = ec.stream_name;
            for (const auto& [media_name, config_name] : encoder_name_map_) {
                if (config_name == ec.stream_name) {
                    pipeline_id = media_name;
                    break;
                }
            }

            HalStreamOverride ov = {};
            snprintf(ov.stream_id, sizeof(ov.stream_id), "%s", pipeline_id.c_str());
            ov.encoder_bitrate = bitrate_diff ? yd.bitrate : ec.bitrate;

            HalStreamOverrideBatch batch = {};
            batch.streams = &ov;
            batch.stream_count = 1;

            int ret = media_ops->override_stream_params(media_ctx_, &batch);
            if (ret < 0) {
                HAL_LOG_ERROR("CameraDaemon: Startup bitrate override failed for %s: %d",
                              ec.stream_name.c_str(), ret);
            } else {
                HAL_LOG_INFO("CameraDaemon: Applied YAML bitrate override for %s: bitrate=%u",
                             ec.stream_name.c_str(), ov.encoder_bitrate);
            }
        }
    }

    return true;
}

void CameraDaemon::resync_encoders_from_media_pipeline() {
    if (!encoder_mgr_ || !hal_loader_) return;
    auto* media_ops = hal_loader_->media();
    if (!media_ops || !media_ctx_ || !media_ops->get_codec_list) return;

    // Pipeline reconfiguration may destroy and replace HalCodecContext objects
    // before this function runs. FROM_MEDIA contexts are borrowed from HAL, so
    // never call unsubscribe/destroy through the old pointers here.
    encoder_mgr_->discard_from_media();

    void* codec_list = nullptr;
    uint32_t codec_count = 0;
    if (media_ops->get_codec_list(media_ctx_, &codec_list, &codec_count) < 0 || !codec_list || codec_count == 0) {
        HAL_LOG_WARNING("CameraDaemon: resync_encoders_from_media_pipeline: get_codec_list failed");
        return;
    }

    void** clist = static_cast<void**>(codec_list);
    for (uint32_t i = 0; i < codec_count; i++) {
        auto* cc = static_cast<HalCodecContext*>(clist[i]);
        if (!cc) continue;
        std::string name(cc->codec_name);
        if (name.empty()) continue;
        if (!encoder_mgr_->create_from_context(name, clist[i])) {
            HAL_LOG_ERROR("CameraDaemon: resync create_from_context failed for %s", name.c_str());
        }
    }
    HAL_LOG_INFO("CameraDaemon: resync_encoders_from_media_pipeline: re-registered %u encoder(s)",
                 codec_count);
}

void CameraDaemon::configure_osd() {
    if (!osd_mgr_ || !encoder_mgr_) return;

    for (auto& ov : config_.osd_overlays) {
        std::string enc_name = ov.stream_name;
        for (const auto& [media_name, config_name] : encoder_name_map_) {
            if (config_name == ov.stream_name) {
                enc_name = media_name;
                break;
            }
        }
        void* codec_ctx = encoder_mgr_->get_codec_ctx(enc_name);
        if (!codec_ctx) {
            HAL_LOG_WARNING("CameraDaemon: No encoder for OSD overlay target '%s' (resolved '%s'), skipping",
                           ov.stream_name.c_str(), enc_name.c_str());
            continue;
        }

        if (ov.type == "text") {
            HalOsdTextOverlay tc{};
            strncpy(tc.base.id, ov.text.c_str(), sizeof(tc.base.id) - 1);
            tc.base.x = ov.x;
            tc.base.y = ov.y;
            tc.base.enabled = true;
            tc.base.z_index = 1;
            strncpy(tc.label, ov.text.c_str(), sizeof(tc.label) - 1);
            tc.text_color = {ov.r, ov.g, ov.b, ov.a};
            tc.font_size = static_cast<float>(ov.font_size);
            osd_mgr_->add_text(codec_ctx, tc);
        } else if (ov.type == "datetime") {
            HalOsdDateTimeOverlay dtc{};
            strncpy(dtc.text.base.id, "datetime", sizeof(dtc.text.base.id) - 1);
            dtc.text.base.x = ov.x;
            dtc.text.base.y = ov.y;
            dtc.text.base.enabled = true;
            dtc.text.base.z_index = 1;
            strncpy(dtc.datetime_format, ov.format.c_str(),
                    sizeof(dtc.datetime_format) - 1);
            dtc.text.text_color = {ov.r, ov.g, ov.b, ov.a};
            dtc.text.font_size = static_cast<float>(ov.font_size);
            osd_mgr_->add_datetime(codec_ctx, dtc);
        } else if (ov.type == "image") {
            HalOsdImageOverlay ic{};
            strncpy(ic.base.id, ov.text.c_str(), sizeof(ic.base.id) - 1);
            ic.base.x = ov.x;
            ic.base.y = ov.y;
            ic.base.enabled = true;
            ic.base.z_index = 1;
            strncpy(ic.image_path, ov.text.c_str(), sizeof(ic.image_path) - 1);
            osd_mgr_->add_image(codec_ctx, ic);
        }
    }
}

bool CameraDaemon::init_encoded_publisher() {
    if (!config_.encoded_pub_enabled) {
        HAL_LOG_INFO("CameraDaemon: Encoded publisher disabled");
        return true;
    }

    encoded_pub_ = std::make_unique<EncodedPublisher>();

    for (auto& ec : config_.encoders) {
        EncodedPublisher::StreamConfig sc;
        sc.name = ec.stream_name;
        sc.codec = ec.codec;
        sc.width = ec.width;
        sc.height = ec.height;
        encoded_pub_->add_stream(sc, config_.encoded_pub_dir);
    }

    // Register built-in RTSP server as local listener (if enabled)
    if (rtsp_server_) {
        auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
        encoded_pub_->add_local_listener(
            [rtsp_weak](const std::string& stream_name, const HalPacketBuffer* packet) {
                if (auto rtsp = rtsp_weak.lock()) {
                    rtsp->on_packet(stream_name, packet);
                }
            });
        HAL_LOG_INFO("CameraDaemon: Built-in RTSP registered as EncodedPublisher local listener");
    }

    // Allow remote plugins to request keyframes via socket control byte
    if (encoder_mgr_) {
        encoded_pub_->set_keyframe_request_cb(
            [this](const std::string& stream_name) {
                std::string enc_name = stream_name;
                for (auto& [mn, cn] : encoder_name_map_) {
                    if (cn == stream_name) { enc_name = mn; break; }
                }
                //HAL_LOG_DEBUG("CameraDaemon: Plugin requested keyframe for %s", stream_name.c_str());
                encoder_mgr_->force_keyframe(enc_name);
            });
    }

    if (!encoded_pub_->start()) {
        HAL_LOG_ERROR("CameraDaemon: Encoded publisher failed to start");
        encoded_pub_.reset();
        return true;  // Non-fatal
    }

    return true;
}

bool CameraDaemon::init_rtsp() {
    if (!config_.rtsp_enabled) {
        HAL_LOG_INFO("CameraDaemon: RTSP server disabled");
        return true;
    }

    rtsp_server_ = std::make_shared<RtspServer>();

    // Register each encoder stream as an RTSP endpoint
    for (auto& ec : config_.encoders) {
        RtspServer::StreamInfo si;
        si.name = ec.stream_name;
        si.codec = ec.codec;
        si.width = ec.width;
        si.height = ec.height;
        si.fps = ec.fps;
        rtsp_server_->add_stream(si);
    }

    // Register audio stream if config enables audio
    if (config_.audio.enabled) {
        RtspServer::StreamInfo asi;
        asi.name = "audio_capture";
        asi.codec = "pcm";
        asi.is_audio = true;
        asi.sample_rate = config_.audio.sample_rate;
        asi.channels = config_.audio.channels;
        rtsp_server_->add_stream(asi);
        // Enable multi-track: video streams will carry audio in SDP
        rtsp_server_->set_audio_info("audio_capture");
    }

    // Wire keyframe request: when PLAY starts, force IDR for instant decode
    rtsp_server_->set_keyframe_request_cb(
        [this](const std::string& stream_name) {
            if (encoder_mgr_) {
                std::string enc_name = stream_name;
                for (auto& [mn, cn] : encoder_name_map_) {
                    if (cn == stream_name) { enc_name = mn; break; }
                }
                HAL_LOG_INFO("RTSP: Requesting keyframe for %s", stream_name.c_str());
                encoder_mgr_->force_keyframe(enc_name);
            }
        });

    if (!rtsp_server_->start(config_.rtsp_port)) {
        HAL_LOG_ERROR("CameraDaemon: RTSP server failed to start on port %d",
                     config_.rtsp_port);
        rtsp_server_.reset();
        return true;  // Non-fatal
    }

    return true;
}

bool CameraDaemon::init_ai_overlay() {
    if (!config_.ai_overlay_enabled) {
        HAL_LOG_INFO("CameraDaemon: AI overlay disabled");
        return true;
    }

    AiOverlayConfig cfg;
    cfg.enabled             = true;
    cfg.event_bus_endpoint  = config_.ai_overlay_event_bus_endpoint;
    cfg.topic_prefix        = config_.ai_overlay_topic_prefix;
    cfg.draw_detections     = true;
    cfg.draw_labels         = config_.ai_overlay_draw_labels;
    cfg.draw_confidence     = config_.ai_overlay_draw_confidence;
    cfg.draw_landmarks      = config_.ai_overlay_draw_landmarks;
    cfg.enable_face_blur    = config_.ai_overlay_enable_face_blur;
    cfg.box_thickness       = config_.ai_overlay_box_thickness;
    cfg.draw_ops            = hal_loader_->draw();

    // Stream mapping: inference stream_id → display encoder stream.
    if (!config_.ai_overlay_stream_map.empty()) {
        cfg.stream_map = config_.ai_overlay_stream_map;
    } else {
        // Auto-generate: map every configured stream → first encoder stream
        std::string primary_encoder;
        if (!config_.encoders.empty()) {
            primary_encoder = config_.encoders[0].stream_name;
        }
        if (!primary_encoder.empty()) {
            for (auto& s : config_.streams) {
                cfg.stream_map[s.name] = primary_encoder;
            }
            cfg.stream_map["cam0_main"] = primary_encoder;
            cfg.stream_map["cam0_sub"]  = primary_encoder;
            cfg.stream_map["ai"]        = primary_encoder;
        }
    }

    for (auto& [k, v] : cfg.stream_map) {
        HAL_LOG_INFO("CameraDaemon: AI overlay stream_map: %s → %s", k.c_str(), v.c_str());
    }

    ai_overlay_ = std::make_unique<AiOverlaySubscriber>(cfg);
    return ai_overlay_->start();
}

void CameraDaemon::register_subscribers() {
    // Registration order matters: FrameRouter dispatches in registration order.
    // In FROM_MEDIA mode with auto-feed enabled, the media pipeline drives
    // encoders directly; FrameRouter handles FD subscribers.

    bool auto_feed = encoder_auto_feed_enabled_.load();

    for (auto& s : config_.streams) {
        bool has_encoder = false;
        for (auto& ec : config_.encoders) {
            if (ec.stream_name == s.name) {
                has_encoder = true;
                break;
            }
        }
        // In FROM_MEDIA mode, check encoder_mgr using mapped (media pipeline) names
        if (!has_encoder && encoder_mgr_) {
            has_encoder = encoder_mgr_->has_encoder(s.name);
            if (!has_encoder) {
                for (auto& [media_name, config_name] : encoder_name_map_) {
                    if (config_name == s.name) {
                        has_encoder = encoder_mgr_->has_encoder(media_name);
                        break;
                    }
                }
            }
        }

        // --- Priority 1: FD subscriber (clean frame) ---
        if (fd_pub_) {
            std::string sname = s.name;
            frame_router_->subscribe(s.name, "fd_" + s.name,
                [this, sname](ManagedFrame* mf) {
                    fd_pub_->on_frame(sname, mf);
                });
        }

        // --- Priority 2: Encoder subscriber (AI overlay → OSD → encode → FPS update) ---
        // Skip in media pipeline auto_feed mode — encoder gets frames from pipeline directly
        if (!auto_feed && has_encoder && encoder_mgr_) {
            std::string sname = s.name;
            std::string enc_name = s.name;
            for (const auto& [media_name, config_name] : encoder_name_map_) {
                if (config_name == sname) {
                    enc_name = media_name;
                    break;
                }
            }
            fps_trackers_[sname] = FpsTracker{};
            frame_router_->subscribe(s.name, "encoder_" + s.name,
                [this, sname, enc_name](ManagedFrame* mf) {
                    if (ai_overlay_) {
                        ai_overlay_->apply_overlay(sname, &mf->frame);
                    }
                    encoder_mgr_->encode_frame(enc_name, &mf->frame);
                    frame_router_->release(mf);

                    // fps_trackers_ is pre-populated at init; no concurrent resize
                    auto& ft = fps_trackers_.at(sname);
                    if (ft.start_time == 0) ft.start_time = time(nullptr);
                    ft.frame_count++;

                    if (osd_mgr_ && osd_enabled_streams_.count(sname) &&
                        ft.frame_count >= 30 && (ft.frame_count % 10) == 0) {
                        void* codec_ctx = encoder_mgr_->get_codec_ctx(enc_name);
                        if (codec_ctx) {
                            time_t elapsed = time(nullptr) - ft.start_time;
                            double fps = (elapsed > 0)
                                ? (double)ft.frame_count / (double)elapsed : 0.0;
                            char label[64];
                            snprintf(label, sizeof(label), "FPS: %.1f", fps);
                            osd_mgr_->update_text(codec_ctx, "fps_0", label);
                        }
                    }
                });
        }
    }
}

/* ========== Profile management ========== */

std::string CameraDaemon::get_current_profile() const {
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) return "";

    char* name = nullptr;
    int ret = media_ops->get_current_profile(media_ctx_, &name);
    if (ret < 0 || !name) {
        HAL_LOG_WARNING("CameraDaemon: get_current_profile failed: %d", ret);
        return "";
    }
    std::string result(name);
    return result;
}

std::vector<std::string> CameraDaemon::list_profiles() const {
    std::vector<std::string> result;
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) return result;

    char* profile_list[64] = {};
    uint32_t count = 0;
    int ret = media_ops->get_profile_list(media_ctx_, profile_list, &count);
    if (ret < 0) {
        HAL_LOG_WARNING("CameraDaemon: get_profile_list failed: %d", ret);
        return result;
    }
    for (uint32_t i = 0; i < count && i < 64; i++) {
        if (profile_list[i]) {
            result.emplace_back(profile_list[i]);
        }
    }
    return result;
}

bool CameraDaemon::get_sensor_info(uint32_t sensor_index, HalVideoSensorModuleInfo* info) const {
    if (!info) return false;

    auto* ops = hal_loader_ ? hal_loader_->video() : nullptr;
    if (!ops || !ops->get_sensor_module_info) {
        HAL_LOG_WARNING("CameraDaemon: get_sensor_module_info not available");
        return false;
    }

    void* ctx = video_source_ ? video_source_->video_ctx() : nullptr;
    if (!ctx) {
        HAL_LOG_WARNING("CameraDaemon: No video context for sensor info query");
        return false;
    }

    std::memset(info, 0, sizeof(*info));
    info->i2c_bus = -1;
    info->sensor_pixel_format = -1;

    int ret = ops->get_sensor_module_info(ctx, sensor_index, info);
    if (ret != 0) {
        HAL_LOG_WARNING("CameraDaemon: get_sensor_module_info failed for index %u: %d",
                        sensor_index, ret);
        return false;
    }

    return true;
}

void CameraDaemon::init_mcu_context() {
    if (!hal_loader_ || !hal_loader_->has_led()) return;
    if (hal_loader_->mcu_ctx()) return;
    if (config_.lens_bridge_lib.empty()) return;

    // Proactively initialize MCU UART context via lens bridge,
    // so LED/IR-cut/Sensor ops work immediately without waiting for
    // an external gRPC lens init request from device-control.
    void* bridge = dlopen(config_.lens_bridge_lib.c_str(), RTLD_NOW);
    if (!bridge) {
        HAL_LOG_WARNING("CameraDaemon: cannot dlopen lens bridge for MCU init");
        return;
    }

    using IoInitFn = int(*)(const char*, uint32_t, uint32_t);
    auto io_init = (IoInitFn)dlsym(bridge, "hal_bridge_io_init");
    if (!io_init) {
        HAL_LOG_WARNING("CameraDaemon: hal_bridge_io_init not found in lens bridge");
        return;
    }

    const char* serial_dev = config_.lens_serial_device.empty()
                             ? "/dev/ttyS0" : config_.lens_serial_device.c_str();
    uint32_t baud = config_.lens_baud_rate > 0 ? config_.lens_baud_rate : 921600;
    uint32_t timeout = config_.lens_timeout_ms > 0 ? config_.lens_timeout_ms : 1000;

    int ret = io_init(serial_dev, baud, timeout);
    if (ret != 0) {
        HAL_LOG_WARNING("CameraDaemon: MCU context init failed (io_init=%d)", ret);
        return;
    }

    // Now share the MCU context with HAL loader for LED/IR-cut/Sensor ops
    try_share_lens_mcu_ctx();
}

void CameraDaemon::try_share_lens_mcu_ctx() {
    if (!hal_loader_ || !hal_loader_->has_led()) return;
    if (config_.lens_bridge_lib.empty()) return;
    void* bridge = dlopen(config_.lens_bridge_lib.c_str(), RTLD_NOW);
    if (!bridge) return;
    using GetMcuCtxFn = void*(*)();
    auto fn = (GetMcuCtxFn)dlsym(bridge, "hal_bridge_get_mcu_ctx");
    if (fn) {
        void* ctx = fn();
        if (ctx && ctx != hal_loader_->mcu_ctx()) {
            hal_loader_->set_mcu_ctx(ctx);
            HAL_LOG_INFO("CameraDaemon: LED/IR-cut sharing lens bridge MCU context");
        }
    }
}

bool CameraDaemon::set_ircut(uint32_t mode) {
    if (!hal_loader_ || !hal_loader_->has_led()) {
        HAL_LOG_ERROR("CameraDaemon: LED/IR-cut HAL not loaded");
        return false;
    }
    try_share_lens_mcu_ctx();
    auto* ops = hal_loader_->led();
    void* ctx = hal_loader_->mcu_ctx();
    if (!ops || !ops->ircut_set_mode || !ctx) {
        HAL_LOG_ERROR("CameraDaemon: IR-cut ops or MCU context not available");
        return false;
    }
    HalIrCutMode m = (mode == 1) ? HAL_IRCUT_NIGHT : HAL_IRCUT_DAY;
    int ret = ops->ircut_set_mode(ctx, m);
    if (ret != HAL_OK) {
        HAL_LOG_ERROR("CameraDaemon: ircut_set_mode failed: %d", ret);
        return false;
    }
    HAL_LOG_INFO("CameraDaemon: IR-cut mode set to %s", mode == 1 ? "night" : "day");
    invalidate_autofocus_anchor("IR-cut mode changed");
    return true;
}

bool CameraDaemon::start_autofocus_one_shot(uint64_t* job_id, std::string* error) {
#ifdef HAS_GRPC
    if (autofocus_controller_) return autofocus_controller_->start_one_shot(job_id, error);
#endif
    if (error) *error = "autofocus controller unavailable";
    return false;
}

bool CameraDaemon::start_autofocus_zoom_follow(float ratio, uint64_t* job_id,
                                                std::string* error) {
#ifdef HAS_GRPC
    if (autofocus_controller_)
        return autofocus_controller_->start_zoom_follow(ratio, job_id, error);
#endif
    if (error) *error = "autofocus controller unavailable";
    return false;
}

bool CameraDaemon::cancel_autofocus(uint64_t job_id, std::string* error) {
#ifdef HAS_GRPC
    if (autofocus_controller_) return autofocus_controller_->cancel(job_id, error);
#endif
    if (error) *error = "autofocus controller unavailable";
    return false;
}

void CameraDaemon::invalidate_autofocus_anchor(const std::string& reason) {
#ifdef HAS_GRPC
    if (autofocus_controller_) autofocus_controller_->invalidate_anchor(reason);
#else
    (void)reason;
#endif
}

AutofocusStatus CameraDaemon::get_autofocus_status() const {
#ifdef HAS_GRPC
    if (autofocus_controller_) return autofocus_controller_->status();
#endif
    AutofocusStatus status;
    status.state = AutofocusState::Failed;
    status.error_code = HAL_ERR_NOT_READY;
    status.message = "autofocus controller unavailable";
    return status;
}

bool CameraDaemon::get_ircut(uint32_t& mode) {
    if (!hal_loader_ || !hal_loader_->has_led()) return false;
    try_share_lens_mcu_ctx();
    auto* ops = hal_loader_->led();
    void* ctx = hal_loader_->mcu_ctx();
    if (!ops || !ops->ircut_get_mode || !ctx) return false;
    HalIrCutMode m;
    int ret = ops->ircut_get_mode(ctx, &m);
    if (ret != HAL_OK) return false;
    mode = static_cast<uint32_t>(m);
    return true;
}

bool CameraDaemon::set_led_duty(uint32_t led_id, uint32_t duty_percent) {
    if (!hal_loader_ || !hal_loader_->has_led()) {
        HAL_LOG_ERROR("CameraDaemon: LED HAL not loaded");
        return false;
    }
    try_share_lens_mcu_ctx();
    auto* ops = hal_loader_->led();
    void* ctx = hal_loader_->mcu_ctx();
    if (!ops || !ops->led_set_duty || !ctx) {
        HAL_LOG_ERROR("CameraDaemon: LED ops or MCU context not available");
        return false;
    }
    if (duty_percent > 100) duty_percent = 100;
    int ret = ops->led_set_duty(ctx, static_cast<uint8_t>(led_id),
                                static_cast<uint8_t>(duty_percent));
    if (ret != HAL_OK) {
        HAL_LOG_ERROR("CameraDaemon: led_set_duty(id=%u, duty=%u) failed: %d", led_id, duty_percent, ret);
        return false;
    }
    HAL_LOG_INFO("CameraDaemon: LED %u duty set to %u%%", led_id, duty_percent);
    return true;
}

bool CameraDaemon::get_led_duty(uint32_t led_id, uint32_t& duty_percent) {
    if (!hal_loader_ || !hal_loader_->has_led()) return false;
    try_share_lens_mcu_ctx();
    auto* ops = hal_loader_->led();
    void* ctx = hal_loader_->mcu_ctx();
    if (!ops || !ops->led_get_duty || !ctx) return false;
    uint8_t duty = 0;
    int ret = ops->led_get_duty(ctx, static_cast<uint8_t>(led_id), &duty);
    if (ret != HAL_OK) return false;
    duty_percent = duty;
    return true;
}

bool CameraDaemon::get_device_hardware_status(aipc::camera::DeviceHardwareStatus& status) {
    status.set_success(true);
    try_share_lens_mcu_ctx();
    void* ctx = hal_loader_ ? hal_loader_->mcu_ctx() : nullptr;

    // Sensor readings
    if (hal_loader_ && hal_loader_->has_sensor() && ctx) {
        auto* sensor_ops = hal_loader_->sensor();
        HalAdcValue pd_val{}, temp_val{}, ain_val{};
        if (sensor_ops->pd_get && sensor_ops->pd_get(ctx, &pd_val) == HAL_OK) {
            status.set_light_sensor_mv(pd_val.mv);
            status.set_light_sensor_lux(pd_val.milli);
        }
        if (sensor_ops->temp_get && sensor_ops->temp_get(ctx, &temp_val) == HAL_OK) {
            status.set_mcu_temp_millic(temp_val.milli);
        }
        if (sensor_ops->ain_get && sensor_ops->ain_get(ctx, &ain_val) == HAL_OK) {
            status.set_ain_mv(ain_val.mv);
        }
    }

    // MCU version
    if (hal_loader_ && hal_loader_->has_mcu() && ctx) {
        auto* mcu_ops = hal_loader_->mcu();
        if (mcu_ops->get_version) {
            HalMcuVersion ver{};
            if (mcu_ops->get_version(ctx, &ver) == HAL_OK) {
                status.set_mcu_version(ver.version_str);
            }
        }
    }

    // LED states
    if (hal_loader_ && hal_loader_->has_led() && ctx) {
        auto* led_ops = hal_loader_->led();
        uint8_t duty = 0;
        if (led_ops->led_get_duty) {
            if (led_ops->led_get_duty(ctx, 0, &duty) == HAL_OK) {
                status.set_white_light_duty(duty);
            }
            if (led_ops->led_get_duty(ctx, 1, &duty) == HAL_OK) {
                status.set_ir_led_duty(duty);
            }
        }
        if (led_ops->ircut_get_mode) {
            HalIrCutMode m;
            if (led_ops->ircut_get_mode(ctx, &m) == HAL_OK) {
                status.set_ircut_mode(static_cast<uint32_t>(m));
            }
        }
    }

    return true;
}

int CameraDaemon::mcu_raw_request(uint16_t cmd, const uint8_t* payload, uint16_t payload_len,
                                   uint8_t* response, uint16_t response_size, uint16_t& response_len) {
    if (!hal_loader_ || !hal_loader_->has_mcu()) {
        HAL_LOG_ERROR("CameraDaemon: MCU HAL not loaded");
        return -1;
    }
    try_share_lens_mcu_ctx();
    auto* mcu_ops = hal_loader_->mcu();
    void* ctx = hal_loader_->mcu_ctx();
    if (!mcu_ops->raw_request || !ctx) {
        HAL_LOG_ERROR("CameraDaemon: MCU raw_request or MCU context not available");
        return -1;
    }
    return mcu_ops->raw_request(ctx, cmd, payload, payload_len,
                                response, response_size, &response_len);
}

#ifdef HAS_GRPC
void CameraDaemon::get_stream_status(aipc::camera::GetStreamStatusResponse& response) {
    std::shared_lock<std::shared_mutex> lock(op_mu_);

    // Frame-aware status thresholds. The whole point of this handler is that an
    // encoder existing in the map is necessary but NOT sufficient — set_profile()
    // can return success while the pipeline silently emits zero frames (the
    // black-screen failure mode). Grade by real frame timing instead.
    constexpr uint64_t kStallThresholdMs = 3000;  // no frames this long → stalled
    constexpr uint64_t kStartupGraceMs   = 5000;  // suppress stall while younger than this

    for (const auto& ec : config_.encoders) {
        std::string codec = ec.codec;
        uint32_t width = ec.width;
        uint32_t height = ec.height;
        uint32_t fps = ec.fps;
        uint32_t bitrate = ec.bitrate;
        uint32_t gop = ec.gop;

        auto* info = response.add_streams();
        info->set_stream_id(ec.stream_name);

        auto publish_config = [&]() {
            info->set_codec(codec);
            info->set_width(width);
            info->set_height(height);
            info->set_fps(fps);
            info->set_bitrate_bps(bitrate);
            info->set_gop(gop);
        };

        if (!encoder_mgr_) {
            publish_config();
            info->set_status("stopped");
            info->set_has_encoder(false);
            info->set_ms_since_last_frame(UINT64_MAX);  // unknown → JSON null
            continue;
        }

        // Resolve media pipeline name for FROM_MEDIA mode
        std::string enc_name = ec.stream_name;
        for (const auto& [media_name, config_name] : encoder_name_map_) {
            if (config_name == ec.stream_name) {
                enc_name = media_name;
                break;
            }
        }

        if (!encoder_mgr_->has_encoder(enc_name)) {
            publish_config();
            info->set_has_encoder(false);
            info->set_status("stopped");
            info->set_ms_since_last_frame(UINT64_MAX);  // unknown → JSON null
            continue;
        }

        void* codec_ctx = encoder_mgr_->get_codec_ctx(enc_name);
        auto* codec_ops = encoder_mgr_->ops();
        if (codec_ctx && codec_ops && codec_ops->get_current_config) {
            HalCodecConfig cur{};
            if (codec_ops->get_current_config(codec_ctx, &cur) == HAL_OK) {
                if (cur.width > 0) width = cur.width;
                if (cur.height > 0) height = cur.height;
                if (cur.framerate > 0) fps = cur.framerate;
                if (cur.bitrate > 0) bitrate = cur.bitrate;
                if (cur.intra_pic_rate > 0) gop = cur.intra_pic_rate;
                if (cur.packet_type == HAL_PACKET_TYPE_H265) {
                    codec = "h265";
                } else if (cur.packet_type == HAL_PACKET_TYPE_H264) {
                    codec = "h264";
                }
            }
        }

        publish_config();
        info->set_has_encoder(true);
        // measured_fps not yet instrumented (rolling window is a follow-up);
        // surface the field as 0 = unknown so the API contract is stable.
        info->set_measured_fps(0);

        uint64_t ms = encoder_mgr_->ms_since_last_packet(enc_name);
        info->set_ms_since_last_frame(ms);  // UINT64_MAX sentinel → JSON null on the Go side

        bool stalled = encoder_mgr_->is_stream_stalled(enc_name, kStallThresholdMs, kStartupGraceMs);
        bool seen    = encoder_mgr_->seen_first_packet(enc_name);

        if (stalled) {
            info->set_status("stalled");
            if (ms != UINT64_MAX) {
                info->set_status_detail("no encoded frames for " +
                                        std::to_string(ms / 1000) + "s");
            } else if (!seen) {
                info->set_status_detail("encoder created but never produced a frame");
            } else {
                info->set_status_detail("no encoded frames");
            }
        } else if (!seen) {
            // Inside startup grace and no first packet yet — pipeline is still
            // spinning up (typical right after a profile switch).
            info->set_status("starting");
            info->set_status_detail("waiting for first encoded frame");
        } else {
            info->set_status("active");
        }
    }
}

void CameraDaemon::add_stream(const aipc::camera::AddStreamRequest& request,
                               aipc::camera::StreamOperationResponse& response) {
    if (!runtime_stream_reconfiguration_enabled()) {
        HAL_LOG_WARNING("CameraDaemon: Runtime AddStream blocked by safety gate");
        response.set_success(false);
        response.set_message(
            "Runtime stream reconfiguration is temporarily disabled; "
            "set AIPC_ALLOW_RUNTIME_STREAM_RECONFIG=1 to opt in");
        return;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);
    const std::string& stream_id = request.stream_id();

    if (stream_id.empty()) {
        response.set_success(false);
        response.set_message("stream_id is required");
        return;
    }

    // Check if stream already exists (and is enabled)
    for (auto& ec : config_.encoders) {
        if (ec.stream_name == stream_id) {
            if (ec.enabled) {
                response.set_success(false);
                response.set_message("Stream already exists: " + stream_id);
                return;
            }
            // Stream exists but is disabled — re-enable it
            HAL_LOG_INFO("CameraDaemon: Re-enabling disabled stream '%s'", stream_id.c_str());
            ec.enabled = true;

            // Build encoder override that includes this stream and reinit pipeline
            auto* mops = hal_loader_ ? hal_loader_->media() : nullptr;
            if (!mops || !media_ctx_) {
                response.set_success(false);
                response.set_message("Media pipeline not available");
                return;
            }

            lock.unlock();

            // Destroy existing encoder state before pipeline reinit
            encoder_mgr_->destroy_all();

            // Build full encoder override JSON including the re-enabled stream
            // encoder_name_map_: media_sink_name → config_stream_name (e.g. "sink0" → "main")
            std::ostringstream oss;
            oss << "[";
            bool first = true;
            for (const auto& [media_name, cfg_name] : encoder_name_map_) {
                for (const auto& e : config_.encoders) {
                    if (e.stream_name == cfg_name && e.enabled) {
                        if (!first) oss << ",";
                        oss << "{\"stream_id\":\"" << media_name << "\","
                            << "\"width\":" << e.width << ","
                            << "\"height\":" << e.height << ","
                            << "\"framerate\":" << e.fps << "}";
                        first = false;
                        break;
                    }
                }
            }
            // Add the re-enabled stream (may not be in encoder_name_map_ yet)
            bool already_in_map = false;
            for (const auto& [mn, cn] : encoder_name_map_) {
                if (cn == stream_id) { already_in_map = true; break; }
            }
            if (!already_in_map) {
                // Assign sink name: main=sink0, sub=sink1, third=sink2
                std::string sink_name = stream_id;
                static const std::unordered_map<std::string, std::string> name_map = {
                    {"main", "sink0"}, {"sub", "sink1"}, {"third", "sink2"}
                };
                auto it = name_map.find(stream_id);
                if (it != name_map.end()) sink_name = it->second;
                if (!first) oss << ",";
                oss << "{\"stream_id\":\"" << sink_name << "\","
                    << "\"width\":" << ec.width << ","
                    << "\"height\":" << ec.height << ","
                    << "\"framerate\":" << ec.fps << "}";
                encoder_name_map_[sink_name] = stream_id;
            }
            oss << "]";

            std::string overrides_json = oss.str();
            HAL_LOG_INFO("CameraDaemon: Re-enable stream '%s' with overrides: %s",
                         stream_id.c_str(), overrides_json.c_str());

            // Reinit pipeline with updated stream layout
            HalMediaConfig mcfg{};
            mcfg.config_path = config_.media_config_path.c_str();
            mcfg.encoder_overrides_json = overrides_json.c_str();
            if (!config_.backup_folder_path.empty())
                mcfg.backup_folder_path = config_.backup_folder_path.c_str();

            // Destroy old media context and reinit
            if (mops->stop) mops->stop(media_ctx_);
            mops->deinit(media_ctx_);
            media_ctx_ = nullptr;

            int ret = mops->init(&mcfg, &media_ctx_);
            if (ret < 0 || !media_ctx_) {
                HAL_LOG_ERROR("CameraDaemon: Pipeline reinit for re-enable '%s' failed: %d, rolling back",
                              stream_id.c_str(), ret);
                ec.enabled = false;

                // Rollback: reinit pipeline without the failed stream
                std::ostringstream rb;
                rb << "[";
                bool rb_first = true;
                for (const auto& [media_name, cfg_name] : encoder_name_map_) {
                    for (const auto& e : config_.encoders) {
                        if (e.stream_name == cfg_name && e.enabled) {
                            if (!rb_first) rb << ",";
                            rb << "{\"stream_id\":\"" << media_name << "\","
                               << "\"width\":" << e.width << ","
                               << "\"height\":" << e.height << ","
                               << "\"framerate\":" << e.fps << "}";
                            rb_first = false;
                            break;
                        }
                    }
                }
                rb << "]";

                HalMediaConfig rb_cfg{};
                rb_cfg.config_path = config_.media_config_path.c_str();
                rb_cfg.encoder_overrides_json = rb.str().c_str();
                if (!config_.backup_folder_path.empty())
                    rb_cfg.backup_folder_path = config_.backup_folder_path.c_str();

                int rb_ret = mops->init(&rb_cfg, &media_ctx_);
                if (rb_ret >= 0 && media_ctx_) {
                    mops->start(media_ctx_);
                    restore_image_config_if_cached();
                    reapply_osd_config_after_pipeline_rebuild("stream re-enable rollback");
                    HAL_LOG_INFO("CameraDaemon: Rollback pipeline restored (2-stream)");
                } else {
                    HAL_LOG_ERROR("CameraDaemon: Rollback pipeline also failed: %d", rb_ret);
                }

                response.set_success(false);
                response.set_message("Pipeline reinit failed: " + std::to_string(ret));
                return;
            }

            // Re-discover codecs from new pipeline
            void* codec_list = nullptr;
            uint32_t codec_count = 0;
            mops->get_codec_list(media_ctx_, &codec_list, &codec_count);
            if (codec_list && codec_count > 0) {
                void** clist = static_cast<void**>(codec_list);
                for (uint32_t i = 0; i < codec_count; i++) {
                    auto* cc = static_cast<HalCodecContext*>(clist[i]);
                    std::string name(cc->codec_name);
                    if (!name.empty()) {
                        encoder_mgr_->create_from_context(name, clist[i]);
                    }
                }
            }

            // Update config params from pipeline
            // Save original YAML bitrate values — HAL codec context may have
            // different bitrate (copied from template encoder during injection).
            // We'll apply YAML values via override_stream_params after start.
            uint32_t enc_idx = 0;
            for (uint32_t i = 0; i < codec_count; ) {
                while (enc_idx < config_.encoders.size() && !config_.encoders[enc_idx].enabled)
                    enc_idx++;
                if (enc_idx >= config_.encoders.size()) break;
                auto* cc = static_cast<HalCodecContext*>(static_cast<void**>(codec_list)[i]);
                std::string media_name(cc->codec_name);
                if (!media_name.empty()) {
                    encoder_name_map_[media_name] = config_.encoders[enc_idx].stream_name;
                    auto& e = config_.encoders[enc_idx];
                    e.width = cc->config.width;
                    e.height = cc->config.height;
                    e.fps = cc->config.framerate;
                    // Don't overwrite YAML bitrate with HAL template bitrate
                    e.gop = cc->config.intra_pic_rate;
                    e.codec = (cc->config.packet_type == HAL_PACKET_TYPE_H265) ? "h265" : "h264";
                    HAL_LOG_INFO("CameraDaemon: Re-enabled encoder '%s' → '%s' (%ux%u %s)",
                                 media_name.c_str(), e.stream_name.c_str(),
                                 e.width, e.height, e.codec.c_str());
                    enc_idx++;
                }
                i++;
            }

            // Start pipeline
            if (mops->start) mops->start(media_ctx_);

            // Re-apply transform/image overrides lost during the deinit+init rebuild.
            restore_image_config_if_cached();

            // Apply YAML bitrate overrides for all enabled streams after reinit
            {
                std::vector<HalStreamOverride> overrides;
                for (const auto& enc : config_.encoders) {
                    if (!enc.enabled || enc.bitrate == 0) continue;
                    std::string pipeline_id = enc.stream_name;
                    for (const auto& [media_name, config_name] : encoder_name_map_) {
                        if (config_name == enc.stream_name) {
                            pipeline_id = media_name;
                            break;
                        }
                    }
                    HalStreamOverride ov = {};
                    snprintf(ov.stream_id, sizeof(ov.stream_id), "%s", pipeline_id.c_str());
                    ov.encoder_bitrate = enc.bitrate;
                    overrides.push_back(ov);
                }
                if (!overrides.empty()) {
                    HalStreamOverrideBatch batch = {};
                    batch.streams = overrides.data();
                    batch.stream_count = (uint32_t)overrides.size();
                    int br_ret = mops->override_stream_params(media_ctx_, &batch);
                    if (br_ret < 0) {
                        HAL_LOG_WARNING("CameraDaemon: Bitrate overrides after reinit failed: %d", br_ret);
                    } else {
                        for (const auto& ov : overrides) {
                            HAL_LOG_INFO("CameraDaemon: Applied bitrate override after reinit for '%s': %u",
                                         ov.stream_id, ov.encoder_bitrate);
                        }
                    }
                }
            }

            reapply_osd_config_after_pipeline_rebuild("stream re-enable");

            response.set_success(true);
            response.set_message("Stream re-enabled: " + stream_id);
            return;
        }
    }

    if (!encoder_mgr_) {
        response.set_success(false);
        response.set_message("Encoder manager not initialized");
        return;
    }

    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (media_ops && media_ctx_) {
        // FROM_MEDIA mode: use add_streams_batch for a single pipeline reconfig.
        // This avoids double pipeline stop/start that wastes DMA buffers.
        if (!media_ops->add_streams_batch && !media_ops->add_codec_stream) {
            response.set_success(false);
            response.set_message("HAL does not support stream addition");
            return;
        }

        // Release op_mu_ before destroy_all / HAL calls to avoid AB-BA deadlock
        // (same pattern as remove_stream — see deadlock analysis there).
        lock.unlock();

        // Destroy all local encoder entries before pipeline reconfig —
        // their HalCodecContext pointers will be invalidated.
        encoder_mgr_->destroy_all();

        // Build configs
        HalMediaAddCodecConfig mac{};
        mac.stream_id = stream_id.c_str();
        mac.codec.type = HAL_CODEC_TYPE_FROM_MEDIA;
        mac.codec.packet_type = (request.codec() == "h265")
                                ? HAL_PACKET_TYPE_H265 : HAL_PACKET_TYPE_H264;
        mac.codec.width      = request.width();
        mac.codec.height     = request.height();
        mac.codec.framerate  = request.fps();
        mac.codec.bitrate    = request.bitrate();
        mac.codec.gop_size   = request.gop();
        mac.codec.rc_mode    = HAL_RC_CBR;

        HalMediaAddVideoConfig mav{};
        mav.stream_id         = stream_id.c_str();
        mav.video.type        = HAL_VIDEO_TYPE_FROM_MEDIA;
        mav.video.width       = request.width();
        mav.video.height      = request.height();
        mav.video.framerate   = request.fps();

        int ret = -1;
        if (media_ops->add_streams_batch) {
            ret = media_ops->add_streams_batch(media_ctx_, &mac, &mav);
            if (ret < 0) {
                HAL_LOG_ERROR("CameraDaemon: add_streams_batch for '%s' failed: %d",
                              stream_id.c_str(), ret);
            } else {
                HAL_LOG_INFO("CameraDaemon: add_streams_batch '%s' OK", stream_id.c_str());
            }
        }
        // Fallback to separate calls if batch not available or failed
        if (ret < 0 && media_ops->add_codec_stream) {
            ret = media_ops->add_codec_stream(media_ctx_, &mac);
            if (ret < 0) {
                HAL_LOG_ERROR("CameraDaemon: add_codec_stream for '%s' failed: %d",
                              stream_id.c_str(), ret);
                response.set_success(false);
                response.set_message("add_codec_stream failed: " + std::to_string(ret));
                return;
            }
            HAL_LOG_INFO("CameraDaemon: add_codec_stream '%s' OK", stream_id.c_str());

            if (media_ops->add_video_stream) {
                int vret = media_ops->add_video_stream(media_ctx_, &mav);
                if (vret < 0) {
                    HAL_LOG_ERROR("CameraDaemon: add_video_stream for '%s' failed: %d, rolling back",
                                  stream_id.c_str(), vret);
                    if (media_ops->remove_codec_stream) {
                        HalMediaRemoveCodecConfig rmc{};
                        rmc.stream_id = stream_id.c_str();
                        media_ops->remove_codec_stream(media_ctx_, &rmc);
                    }
                    response.set_success(false);
                    response.set_message("add_video_stream failed: " + std::to_string(vret));
                    return;
                }
            }
        } else if (ret < 0) {
            response.set_success(false);
            response.set_message("No stream addition API available");
            return;
        }

        // Derive the actual media-pipeline name from the HAL codec list.
        // The new stream is the one whose codec_name is NOT yet in encoder_name_map_.
        std::string media_name = stream_id;
        {
            std::shared_lock<std::shared_mutex> map_lock(op_mu_);
            if (media_ops->get_codec_list) {
                void* codec_list = nullptr;
                uint32_t codec_count = 0;
                if (media_ops->get_codec_list(media_ctx_, &codec_list, &codec_count) >= 0 && codec_list) {
                    void** clist = static_cast<void**>(codec_list);
                    for (uint32_t i = 0; i < codec_count; i++) {
                        auto* cc = static_cast<HalCodecContext*>(clist[i]);
                        if (!cc) continue;
                        std::string name(cc->codec_name);
                        if (encoder_name_map_.find(name) == encoder_name_map_.end()) {
                            media_name = name;
                            break;
                        }
                    }
                }
            }
        }

        // Enable automatic frontend->encoder forwarding for the new stream.
        // Use the media-pipeline name (sink-style) for the auto-feed call.
        if (encoder_auto_feed_enabled_.load()) {
            if (!media_ops->set_encoder_auto_feed_for_stream) {
                HAL_LOG_WARNING("CameraDaemon: auto-feed enabled but HAL lacks per-stream setter; fallback to global behavior");
            } else if (media_ops->set_encoder_auto_feed_for_stream(media_ctx_, media_name.c_str(), true) < 0) {
                HAL_LOG_ERROR("CameraDaemon: set_encoder_auto_feed_for_stream(%s,true) failed",
                              media_name.c_str());
            }
        }

        // Reacquire lock for config_ / encoder_name_map_ mutations
        lock.lock();
        encoder_name_map_[media_name] = stream_id;
        HAL_LOG_INFO("CameraDaemon: encoder_name_map[%s] = %s", media_name.c_str(), stream_id.c_str());

        // Update config
        EncoderCfg new_ec;
        new_ec.stream_name = stream_id;
        new_ec.codec       = request.codec().empty() ? "h264" : request.codec();
        new_ec.width       = request.width();
        new_ec.height      = request.height();
        new_ec.fps         = request.fps();
        new_ec.bitrate     = request.bitrate();
        new_ec.gop         = request.gop();
        new_ec.cbr         = true;
        config_.encoders.push_back(new_ec);

        // Register with EncodedPublisher (UDS socket for WebSocket streaming)
        if (encoded_pub_) {
            EncodedPublisher::StreamConfig esc;
            esc.name   = stream_id;
            esc.codec  = new_ec.codec;
            esc.width  = request.width();
            esc.height = request.height();
            encoded_pub_->add_stream(esc, config_.encoded_pub_dir);
        }

        // Release lock before resync — destroy_all()/subscribe() can block on
        // callbacks that try to acquire op_mu_ (read) via output_fn_.
        lock.unlock();

        // Update video_source_ contexts — add_streams_batch may have rebuilt
        // the pipeline and freed old video contexts, leaving stale pointers.
        if (media_ops->get_video_list && video_source_) {
            void* video_list = nullptr;
            uint32_t video_count = 0;
            if (media_ops->get_video_list(media_ctx_, &video_list, &video_count) >= 0
                && video_list && video_count > 0) {
                video_source_->init_from_context(static_cast<void**>(video_list), video_count);

                bind_video_source_callbacks();
                for (auto& slot : video_source_->streams()) {
                    video_source_->start_stream(slot.name);
                }
            }
        }

        resync_encoders_from_media_pipeline();
        lock.lock();
        HAL_LOG_INFO("CameraDaemon: Stream '%s' added via HAL incremental (no interruption) (%ux%u %s %ubps) media_name=%s",
                     stream_id.c_str(), request.width(), request.height(),
                     new_ec.codec.c_str(), request.bitrate(), media_name.c_str());
        response.set_success(true);
        response.set_message("Stream added (incremental): " + stream_id);
        return;
    }

    // Legacy mode: create a new standalone encoder
    EncoderCfg ec;
    ec.stream_name = stream_id;
    ec.codec = request.codec().empty() ? "h264" : request.codec();
    ec.width = request.width();
    ec.height = request.height();
    ec.fps = request.fps();
    ec.bitrate = request.bitrate();
    ec.gop = request.gop();
    ec.cbr = true;

    HalCodecConfig codec_cfg{};
    codec_cfg.type = HAL_CODEC_TYPE_HW;
    if (ec.codec == "h265") {
        codec_cfg.packet_type = HAL_PACKET_TYPE_H265;
    } else {
        codec_cfg.packet_type = HAL_PACKET_TYPE_H264;
    }
    codec_cfg.path = nullptr;
    codec_cfg.width = ec.width;
    codec_cfg.height = ec.height;
    codec_cfg.format = HAL_PIX_FMT_NV12;
    codec_cfg.framerate = ec.fps;
    codec_cfg.bitrate = ec.bitrate;
    codec_cfg.gop_size = ec.gop;
    codec_cfg.rc_mode = HAL_RC_CBR;

    if (!encoder_mgr_->create(stream_id, codec_cfg)) {
        response.set_success(false);
        response.set_message("Failed to create encoder for " + stream_id);
        return;
    }

    if (!encoder_mgr_->start(stream_id)) {
        HAL_LOG_WARNING("CameraDaemon: Encoder created but failed to start for %s", stream_id.c_str());
    }

    // Add to config
    config_.encoders.push_back(ec);

    // Add stream config for frame routing
    StreamCfg sc;
    sc.name = stream_id;
    sc.width = ec.width;
    sc.height = ec.height;
    sc.fps = ec.fps;
    config_.streams.push_back(sc);

    // Register with encoded publisher for RTSP output
    if (encoded_pub_) {
        EncodedPublisher::StreamConfig esc;
        esc.name = stream_id;
        esc.codec = ec.codec;
        esc.width = ec.width;
        esc.height = ec.height;
        encoded_pub_->add_stream(esc, config_.encoded_pub_dir);
    }

    // Register frame callback if video source is running
    if (video_source_) {
        for (auto& slot : video_source_->streams()) {
            if (slot.name == stream_id) {
                video_source_->set_frame_callback(
                    slot.name,
                    [this, stream_id](const std::string&, HalFrameBuffer* frame) {
                        handle_video_frame_for_routing(stream_id, frame);
                    });
                video_source_->start_stream(slot.name);
                break;
            }
        }
    }

    HAL_LOG_INFO("CameraDaemon: Stream '%s' added (%ux%u %s %ubps)",
                 stream_id.c_str(), ec.width, ec.height, ec.codec.c_str(), ec.bitrate);

    response.set_success(true);
    response.set_message("Stream added: " + stream_id);
}

void CameraDaemon::remove_stream(const std::string& stream_name,
                                  aipc::camera::StreamOperationResponse& response) {
    if (!runtime_stream_reconfiguration_enabled()) {
        HAL_LOG_WARNING("CameraDaemon: Runtime RemoveStream blocked by safety gate");
        response.set_success(false);
        response.set_message(
            "Runtime stream reconfiguration is temporarily disabled; "
            "set AIPC_ALLOW_RUNTIME_STREAM_RECONFIG=1 to opt in");
        return;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);

    if (stream_name == "main") {
        response.set_success(false);
        response.set_message("Cannot remove main stream");
        return;
    }

    if (!encoder_mgr_) {
        response.set_success(false);
        response.set_message("Encoder manager not initialized");
        return;
    }

    // Find and remove from config
    bool found = false;
    for (auto it = config_.encoders.begin(); it != config_.encoders.end(); ++it) {
        if (it->stream_name == stream_name) {
            config_.encoders.erase(it);
            found = true;
            break;
        }
    }

    if (!found) {
        response.set_success(false);
        response.set_message("Stream not found: " + stream_name);
        return;
    }

    // Remove from streams config
    for (auto it = config_.streams.begin(); it != config_.streams.end(); ++it) {
        if (it->name == stream_name) {
            config_.streams.erase(it);
            break;
        }
    }

    // Resolve media pipeline name (e.g. "sub" → "sink1")
    std::string enc_name = stream_name;
    for (const auto& [media_name, config_name] : encoder_name_map_) {
        if (config_name == stream_name) {
            enc_name = media_name;
            break;
        }
    }

    // FROM_MEDIA mode: destroy all local encoder state BEFORE HAL remove calls.
    // Each remove_codec_stream / remove_video_stream can trigger a pipeline reconfig
    // that invalidates HalCodecContext pointers for remaining encoders.  If we
    // destroy_all() afterwards, the unsubscribe() call dereferences stale pointers → crash.
    //
    // CRITICAL: Release op_mu_ before destroy_all / HAL calls to avoid AB-BA deadlock:
    //   Thread A (this): op_mu_(write) → priv->mutex (via unsubscribe)
    //   Thread B (encoder callback): priv->mutex → op_mu_(read, via output_fn_)
    // We already completed config_ edits above, so the lock is no longer needed.
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    lock.unlock();

    if (media_ops && media_ctx_) {
        // Disable auto-feed before removing, so no frames are delivered during teardown
        if (encoder_auto_feed_enabled_.load() && media_ops->set_encoder_auto_feed_for_stream) {
            media_ops->set_encoder_auto_feed_for_stream(media_ctx_, enc_name.c_str(), false);
        }

        // Destroy ALL local encoder entries first — their codec_ctx pointers
        // will be invalidated by the pipeline reconfig below.
        encoder_mgr_->destroy_all();

        // Use batch remove to commit both codec + video removal in a single
        // apply_profile_override_and_refresh, avoiding a double pipeline stop/start.
        if (media_ops->remove_streams_batch) {
            int ret = media_ops->remove_streams_batch(media_ctx_, enc_name.c_str());
            if (ret < 0) {
                HAL_LOG_WARNING("CameraDaemon: remove_streams_batch '%s' failed: %d, trying separate calls",
                                enc_name.c_str(), ret);
                // Fallback to separate calls
                if (media_ops->remove_codec_stream) {
                    HalMediaRemoveCodecConfig rmc{};
                    rmc.stream_id = enc_name.c_str();
                    media_ops->remove_codec_stream(media_ctx_, &rmc);
                }
                if (media_ops->remove_video_stream) {
                    HalMediaRemoveVideoConfig rmv{};
                    rmv.stream_id = enc_name.c_str();
                    media_ops->remove_video_stream(media_ctx_, &rmv);
                }
            } else {
                HAL_LOG_INFO("CameraDaemon: remove_streams_batch '%s' OK", enc_name.c_str());
            }
        } else {
            // No batch API — fall back to separate calls
            if (media_ops->remove_codec_stream) {
                HalMediaRemoveCodecConfig rmc{};
                rmc.stream_id = enc_name.c_str();
                media_ops->remove_codec_stream(media_ctx_, &rmc);
            }
            if (media_ops->remove_video_stream) {
                HalMediaRemoveVideoConfig rmv{};
                rmv.stream_id = enc_name.c_str();
                media_ops->remove_video_stream(media_ctx_, &rmv);
            }
        }
    } else {
        // Non-FROM_MEDIA path: just destroy the one encoder
        encoder_mgr_->destroy(enc_name);
    }

    // Update video_source_ contexts — remove_streams_batch may have rebuilt
    // the pipeline and freed old video contexts, leaving stale pointers.
    if (media_ops && media_ctx_ && video_source_ && media_ops->get_video_list) {
        void* video_list = nullptr;
        uint32_t video_count = 0;
        int vr = media_ops->get_video_list(media_ctx_, &video_list, &video_count);
        if (vr >= 0 && video_list && video_count > 0) {
            video_source_->init_from_context(static_cast<void**>(video_list), video_count);

            // Rebind frame callbacks and re-subscribe for remaining streams.
            bind_video_source_callbacks();
            for (auto& slot : video_source_->streams()) {
                video_source_->start_stream(slot.name);
            }

            // Update config_.streams to match new pipeline
            config_.streams.clear();
            void** vlist = static_cast<void**>(video_list);
            for (uint32_t i = 0; i < video_count; i++) {
                auto* vc = static_cast<HalVideoContext*>(vlist[i]);
                StreamCfg sc;
                sc.name = vc->video_name;
                sc.width = vc->config.width;
                sc.height = vc->config.height;
                sc.fps = vc->config.framerate;
                config_.streams.push_back(sc);
            }
        }
    }

    // Clean up name mapping (reacquire lock briefly)
    {
        std::unique_lock<std::shared_mutex> map_lock(op_mu_);
        for (auto it = encoder_name_map_.begin(); it != encoder_name_map_.end(); ++it) {
            if (it->second == stream_name) {
                encoder_name_map_.erase(it);
                break;
            }
        }
    }

    // Rebuild encoder entries from new codec list (fresh HalCodecContext pointers).
    resync_encoders_from_media_pipeline();
    restore_image_config_if_cached();
    reapply_osd_config_after_pipeline_rebuild("stream removal");

    // EncodedPublisher: close UDS socket and remove stream entry.
    if (encoded_pub_)
        encoded_pub_->remove_stream(stream_name);

    HAL_LOG_INFO("CameraDaemon: Stream '%s' removed (HAL incremental, no interruption to other streams)",
                 stream_name.c_str());

    response.set_success(true);
    response.set_message("Stream removed: " + stream_name);
}
#endif

bool CameraDaemon::backup_profile(const std::string& path) {
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        HAL_LOG_ERROR("CameraDaemon: Cannot backup without media pipeline");
        return false;
    }
    if (!media_ops->backup_current_profile) {
        HAL_LOG_ERROR("CameraDaemon: HAL does not support backup_current_profile");
        return false;
    }

    const char* cpath = path.empty() ? nullptr : path.c_str();
    int ret = media_ops->backup_current_profile(media_ctx_, cpath);
    if (ret < 0) {
        HAL_LOG_ERROR("CameraDaemon: backup_current_profile failed: %d", ret);
        return false;
    }
    HAL_LOG_INFO("CameraDaemon: Profile backed up successfully");
    return true;
}

bool CameraDaemon::switch_profile(const std::string& profile_name, std::string* message) {
    // Throttle gate 1/3 — reject a switch while another is already in flight.
    // op_mu_ is intentionally released around the blocking HAL switch and through
    // the verify/rollback windows below; without this guard a second concurrent
    // profile switch would race the first against a half-rebuilt pipeline and
    // intermittently black-screen. try_lock (non-blocking): a rapid second request
    // is REJECTED rather than queued — this matches the kernel-layer root trigger
    // ("fast toggle called without priming buffer set"), fired by back-to-back
    // STREAMON/OFF before a priming buffer is queued, which corrupts the ISP FE
    // state machine into a below-medialib wedge only a reboot clears. RAII unlock
    // on every return path. (profile_switch_mu_ != pipeline_reconfig_mu_: the
    // encoder reconfig path is a separate pipeline restart, independently guarded.)
    std::unique_lock<std::mutex> sw_lock(profile_switch_mu_, std::try_to_lock);
    if (!sw_lock.owns_lock()) {
        HAL_LOG_WARNING("CameraDaemon: profile switch already in progress — request rejected (throttle gate 1)");
        if (message) *message = "profile switch already in progress, please retry shortly";
        return false;
    }
    std::unique_lock<std::shared_mutex> lock(op_mu_);
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;

    if (!media_ops || !media_ctx_) {
        HAL_LOG_ERROR("CameraDaemon: Cannot switch profile without media pipeline");
        if (message) *message = "media pipeline not initialized";
        return false;
    }

    // Save current profile name for rollback
    std::string prev_profile = get_current_profile();

    // Throttle gate 2/3 — no-op if the requested profile is already active.
    if (profile_name == prev_profile) {
        HAL_LOG_INFO("CameraDaemon: already on profile '%s' — switch skipped (throttle gate 2)",
                     profile_name.c_str());
        if (message) *message = "profile already active";
        return true;
    }

    // Throttle gate 3/3 — minimum interval between switch STARTS. Two same-layout
    // switches fired faster than the pipeline can settle a priming buffer corrupt
    // the ISP FE state machine ("fast toggle called without priming buffer set" →
    // "Received FE interrupt while FE not enabled"), a below-medialib wedge only a
    // reboot clears. Enforce a cooldown so rapid UI taps coalesce into one switch.
    constexpr auto kMinSwitchInterval = std::chrono::milliseconds(5000);
    const auto now = std::chrono::steady_clock::now();
    if (last_switch_start_time_ &&
        (now - *last_switch_start_time_) < kMinSwitchInterval) {
        const auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - *last_switch_start_time_).count();
        HAL_LOG_WARNING("CameraDaemon: profile switch too soon (%lldms < %lldms) — request rejected (throttle gate 3)",
                        static_cast<long long>(since_ms),
                        static_cast<long long>(kMinSwitchInterval.count()));
        if (message) *message = "profile switching too fast, please wait a moment";
        return false;
    }
    if (last_switch_start_time_) {
        const auto since_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - *last_switch_start_time_).count();
        HAL_LOG_INFO("CameraDaemon: profile switch passed cooldown (%lldms >= %lldms) — proceeding (throttle gate 3)",
                     static_cast<long long>(since_ms),
                     static_cast<long long>(kMinSwitchInterval.count()));
    } else {
        HAL_LOG_INFO("CameraDaemon: first profile switch (no cooldown baseline) — proceeding (throttle gate 3)");
    }
    last_switch_start_time_ = now;

    HAL_LOG_INFO("CameraDaemon: Switching to profile '%s' (current: '%s')...",
                 profile_name.c_str(), prev_profile.c_str());

#ifdef HAS_GRPC
    if (autofocus_controller_) {
        autofocus_controller_->stop();
        autofocus_controller_->invalidate_anchor("media profile changed");
    }
#endif

    // 1. Stop current data consumers
    if (encoded_pub_) encoded_pub_->stop();
    if (rtsp_server_) rtsp_server_->stop();
    if (fd_pub_) fd_pub_->stop();

    auto restart_consumers_after_failure = [&]() {
        HAL_LOG_WARNING("CameraDaemon: restarting consumers after failed profile switch");
        if (rtsp_server_ && config_.rtsp_enabled) {
            rtsp_server_.reset();
            init_rtsp();
            if (encoded_pub_) {
                auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
                encoded_pub_->add_local_listener(
                    [rtsp_weak](const std::string& sn, const HalPacketBuffer* pkt) {
                        if (auto rtsp = rtsp_weak.lock()) rtsp->on_packet(sn, pkt);
                    });
            }
        }
        if (encoded_pub_ && !encoded_pub_->start()) {
            HAL_LOG_ERROR("CameraDaemon: failed to restart EncodedPublisher after profile switch failure");
        }
        if (fd_pub_ && !fd_pub_->start()) {
            HAL_LOG_ERROR("CameraDaemon: failed to restart FdPublisher after profile switch failure");
        }
#ifdef HAS_GRPC
        if (autofocus_controller_) {
            autofocus_controller_->update_video_context(video_source_->video_ctx());
            autofocus_controller_->start();
        }
#endif
    };

    // Release op_mu_ before HAL switch_profile — it calls set_profile() which
    // may stop/restart the pipeline, waiting for GStreamer callbacks that take
    // op_mu_ (read) via output_fn_.  Same AB-BA deadlock pattern.
    lock.unlock();

    // 2. Switch profile (MediaLibrary handles pipeline restart internally).
    // force_recycle=false: normal switch — let same-layout take the fast path; the frame verify below
    // catches the 0-frame case and the rollback there uses force_recycle=true.
    int ret = media_ops->switch_profile(media_ctx_, profile_name.c_str(), false);
    if (ret < 0) {
        HAL_LOG_ERROR("CameraDaemon: switch_profile to '%s' failed: %d, rolling back to '%s'",
                      profile_name.c_str(), ret, prev_profile.c_str());
        // Surface a human-readable cause so the RPC layer can give the user a
        // specific toast (thermal restriction vs generic failure). The HAL layer
        // already logged the full thermal/auto-restriction/denoise context.
        if (message) {
            if (ret == HAL_ERR_PROFILE_RESTRICTED) {
                *message = "thermal restricted: AI Denoise gated off; cool device to FULL_PERFORMANCE or pick a denoise-off profile";
            } else if (ret == HAL_ERR_PROFILE_INVALID) {
                *message = "profile '" + profile_name + "' failed validation";
            } else {
                *message = "switch to '" + profile_name + "' failed (ret=" + std::to_string(ret) + ")";
            }
        }
        // Attempt rollback to previous profile. force_recycle=false: a failed forward switch (e.g.
        // thermal PROFILE_IS_RESTRICTED) may not have touched the pipeline, so don't tear down a
        // healthy one. (The verify-fail rollback below uses force_recycle=true instead.)
        if (!prev_profile.empty()) {
            int rb = media_ops->switch_profile(media_ctx_, prev_profile.c_str(), false);
            if (rb < 0) {
                HAL_LOG_ERROR("CameraDaemon: rollback to '%s' also failed: %d", prev_profile.c_str(), rb);
            } else {
                HAL_LOG_INFO("CameraDaemon: rolled back to profile '%s'", prev_profile.c_str());
            }
        }
        restart_consumers_after_failure();
        // A failed forward switch whose rollback is ALSO black leaves the user on a
        // dead pipeline — verify and surface it. get_stream_status() reports the
        // truth independently; this log is the operator-facing signal.
        if (!prev_profile.empty() && !verify_primary_stream_frames(5000)) {
            HAL_LOG_ERROR("CameraDaemon: rollback to '%s' after forward-switch failure "
                          "produces no frames — pipeline degraded; manual restart may be needed",
                          prev_profile.c_str());
        }
        return false;
    }

    // 3. Re-discover stream config from new pipeline contexts
    // Video contexts may have changed (different stream count/resolution)
    lock.lock();
    {
        void* video_list = nullptr;
        uint32_t video_count = 0;
        ret = media_ops->get_video_list(media_ctx_, &video_list, &video_count);
        if (ret >= 0 && video_list && video_count > 0) {
            void** vlist = static_cast<void**>(video_list);
            // Clear and rebuild config_.streams
            auto& vs_streams = video_source_->streams();
            // Update existing entries (matched by order)
            for (size_t i = 0; i < vs_streams.size(); i++) {
                auto* vc = static_cast<HalVideoContext*>(vlist[i]);
                if (i < config_.streams.size()) {
                    if (video_name_map_.count(vs_streams[i].name)) {
                        // Already mapped — update params
                        config_.streams[i].width = vc->config.width;
                        config_.streams[i].height = vc->config.height;
                        config_.streams[i].fps = vc->config.framerate;
                    }
                    HAL_LOG_INFO("CameraDaemon: Post-switch video '%s' (%ux%u@%u)",
                                vs_streams[i].name.c_str(),
                                vc->config.width, vc->config.height, vc->config.framerate);
                }
            }
        }
    }

    {
        void* codec_list = nullptr;
        uint32_t codec_count = 0;
        ret = media_ops->get_codec_list(media_ctx_, &codec_list, &codec_count);
        if (ret >= 0 && codec_list && codec_count > 0) {
            void** clist = static_cast<void**>(codec_list);
            for (size_t i = 0; i < codec_count && i < config_.encoders.size(); i++) {
                auto* cc = static_cast<HalCodecContext*>(clist[i]);
                auto& ec = config_.encoders[i];
                ec.width = cc->config.width;
                ec.height = cc->config.height;
                ec.fps = cc->config.framerate;
                ec.bitrate = cc->config.bitrate;
                ec.gop = cc->config.intra_pic_rate;
                ec.codec = (cc->config.packet_type == HAL_PACKET_TYPE_H265) ? "h265" : "h264";
                HAL_LOG_INFO("CameraDaemon: Post-switch encoder (%ux%u %s %ubps)",
                            ec.width, ec.height, ec.codec.c_str(), ec.bitrate);
            }
        }
    }

    // Release lock before resync — destroy_all()/unsubscribe can wait for
    // encoder callbacks that try to acquire op_mu_ (read).
    lock.unlock();

    resync_encoders_from_media_pipeline();
#ifdef HAS_GRPC
    reapply_osd_config_after_pipeline_rebuild("profile switch");
#endif

    // 4. Restart consumers
    if (rtsp_server_ && config_.rtsp_enabled) {
        rtsp_server_.reset();
        init_rtsp();
        if (encoded_pub_) {
            auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
            encoded_pub_->add_local_listener(
                [rtsp_weak](const std::string& sn, const HalPacketBuffer* pkt) {
                    if (auto rtsp = rtsp_weak.lock()) rtsp->on_packet(sn, pkt);
                });
        }
    }

    if (encoded_pub_) encoded_pub_->start();
    if (fd_pub_) fd_pub_->start();

#ifdef HAS_GRPC
    if (autofocus_controller_) {
        autofocus_controller_->update_video_context(video_source_->video_ctx());
        autofocus_controller_->start();
    }
#endif

    // 5. Post-switch frame verify — the real fix for the black-screen bug.
    // set_profile() can return success while the pipeline silently produces
    // zero encoded frames (denoise VDevice/HEF partial alloc failure, or a HAL
    // bridge reconnect that drops the encoder path). The HAL switch returning
    // >=0 above is therefore NOT proof of live video. Poll the primary stream
    // for real frames; if none arrive within budget, roll back to the previous
    // profile so the player reconnects to a known-good pipeline instead of a
    // frozen black screen. op_mu_ is NOT held here (released above), matching
    // the existing HAL-call windows — no AB-BA with on_packet.
    {
        constexpr uint64_t kVerifyBudgetMs = 5000;  // total time we wait for a frame

        std::string primary_stream;
        bool verified = verify_primary_stream_frames(kVerifyBudgetMs, &primary_stream);
        if (verified) {
            HAL_LOG_INFO("CameraDaemon: post-switch frame verify OK on '%s'",
                         primary_stream.c_str());
        }

        if (!verified) {
            HAL_LOG_ERROR(
                "CameraDaemon: post-switch frame verify FAILED for '%s' "
                "(no frames on '%s' within %ums); rolling back to '%s'",
                profile_name.c_str(), primary_stream.c_str(),
                kVerifyBudgetMs, prev_profile.c_str());

            // Stop consumers before the rollback HAL switch, mirroring the
            // forward flow: switch_profile restarts the pipeline internally and
            // would race running consumer threads. restart_consumers_after_failure()
            // below is NOT safe to call while they're already running
            // (EncodedPublisher::start would leak epoll fds + spawn dup threads).
#ifdef HAS_GRPC
            if (autofocus_controller_) {
                autofocus_controller_->stop();
                autofocus_controller_->invalidate_anchor("post-switch verify rollback");
            }
#endif
            if (encoded_pub_) encoded_pub_->stop();
            if (rtsp_server_) rtsp_server_->stop();
            if (fd_pub_) fd_pub_->stop();

            bool rolled_back = false;
            if (!prev_profile.empty()) {  // mirror the ret<0 rollback guard
                // force_recycle=true: the forward switch left a media graph that reports RUNNING but
                // emits 0 frames (that's why we're here). The same-layout fast path would only reconnect
                // bridges and NOT reset the wedged graph — forcing the full stop/start recycle is what
                // turns a double-black into self-healing instead of requiring a reboot.
                int rb = media_ops->switch_profile(media_ctx_, prev_profile.c_str(), true);
                if (rb < 0) {
                    HAL_LOG_ERROR("CameraDaemon: rollback to '%s' after verify-fail also failed: %d",
                                  prev_profile.c_str(), rb);
                } else {
                    // Pipeline is now prev_profile again; the new-profile codec
                    // contexts registered during the forward resync are dangling —
                    // re-register prev_profile's contexts before restarting consumers.
                    // (The ret<0 path skips this: a failed forward switch may not
                    // have touched the pipeline, so old contexts can stay valid.)
                    resync_encoders_from_media_pipeline();
#ifdef HAS_GRPC
                    reapply_osd_config_after_pipeline_rebuild("profile switch rollback");
#endif
                    rolled_back = true;
                    HAL_LOG_INFO("CameraDaemon: rolled back to profile '%s' after verify-fail",
                                 prev_profile.c_str());
                }
            }
            restart_consumers_after_failure();

            // Verify the rolled-back pipeline ALSO produces frames. The rollback can
            // itself fail to produce video (e.g. CMA further fragmented by the double
            // pipeline restart), leaving the user on a black prev_profile. Without this
            // check we'd report a misleading "rolled back to Y". get_stream_status()
            // already reports the truth independently; this adds one more clean-switch
            // attempt and an honest cause string for the RPC/UI toast.
            bool rb_verified = !rolled_back || verify_primary_stream_frames(kVerifyBudgetMs);
            if (rolled_back && !rb_verified) {
                HAL_LOG_ERROR("CameraDaemon: rollback to '%s' after verify-fail ALSO produces "
                              "no frames — pipeline degraded; manual restart may be needed",
                              prev_profile.c_str());
            }

            if (message) {
                if (!rolled_back) {
                    *message = "profile '" + profile_name + "' produced no video frames within " +
                               std::to_string(kVerifyBudgetMs / 1000) + "s";
                } else if (rb_verified) {
                    *message = "profile '" + profile_name + "' produced no video frames within " +
                               std::to_string(kVerifyBudgetMs / 1000) +
                               "s; rolled back to '" + prev_profile + "'";
                } else {
                    *message = "profile '" + profile_name + "' produced no video frames within " +
                               std::to_string(kVerifyBudgetMs / 1000) + "s; rolled back to '" +
                               prev_profile + "' but it is also producing no frames; "
                               "pipeline may need a manual restart";
                }
            }
            return false;  // skip persist_profile_config — new profile did not take
        }
    }

    HAL_LOG_INFO("CameraDaemon: Profile switched to '%s' successfully", profile_name.c_str());
    // Persist the now-active profile name so the change survives restart/deploy/
    // OS-upgrade. Best-effort: the HAL switch already succeeded; only the
    // restart-survival mirror is at stake (a failure is logged, not fatal).
    persist_profile_config(profile_name);
    return true;
}

bool CameraDaemon::verify_primary_stream_frames(uint64_t budget_ms, std::string* primary_out) {
    constexpr uint64_t kVerifyPollMs  = 250;
    constexpr uint64_t kVerifyFreshMs = 3000;  // a frame within this window counts as live

    // Resolve the primary stream (first configured encoder → media name).
    std::string primary_stream;
    if (!config_.encoders.empty()) {
        primary_stream = config_.encoders.front().stream_name;
        for (const auto& [media_name, config_name] : encoder_name_map_) {
            if (config_name == primary_stream) {
                primary_stream = media_name;
                break;
            }
        }
    }
    if (primary_out) *primary_out = primary_stream;

    // No resolvable primary stream → nothing to measure; treat as verified so we
    // don't block a switch on a config we can't introspect (mirrors the original
    // `verified = primary_stream.empty()` short-circuit).
    if (primary_stream.empty()) {
        return true;
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (encoder_mgr_->seen_first_packet(primary_stream) &&
            encoder_mgr_->ms_since_last_packet(primary_stream) < kVerifyFreshMs) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kVerifyPollMs));
    }
    HAL_LOG_WARNING("CameraDaemon: frame verify timed out on '%s' (no fresh frames within %ums)",
                    primary_stream.c_str(), budget_ms);
    return false;
}

#ifdef HAS_GRPC
bool CameraDaemon::reconfigure_pipeline(const aipc::camera::ReconfigurePipelineRequest& request,
                                         aipc::camera::ReconfigurePipelineResponse& response) {
    if (!runtime_stream_reconfiguration_enabled()) {
        HAL_LOG_WARNING("CameraDaemon: Runtime ReconfigurePipeline blocked by safety gate");
        response.set_success(false);
        response.set_message(
            "Runtime stream reconfiguration is temporarily disabled; "
            "set AIPC_ALLOW_RUNTIME_STREAM_RECONFIG=1 to opt in");
        return false;
    }

    std::unique_lock<std::mutex> reconfig_lock(pipeline_reconfig_mu_, std::try_to_lock);
    if (!reconfig_lock.owns_lock()) {
        HAL_LOG_WARNING("CameraDaemon: Encoder/pipeline reconfiguration already in progress");
        response.set_success(false);
        response.set_message("Encoder/pipeline reconfiguration already in progress");
        return false;
    }

    std::unique_lock<std::shared_mutex> lock(op_mu_);
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (!media_ops || !media_ctx_) {
        HAL_LOG_ERROR("CameraDaemon: Cannot reconfigure pipeline without media pipeline");
        response.set_success(false);
        response.set_message("No media pipeline available");
        return false;
    }
    if (!media_ops->reconfigure_pipeline) {
        response.set_success(false);
        response.set_message("Pipeline reconfiguration not supported");
        return false;
    }

    const auto& req_streams = request.streams();
    if (req_streams.size() == 0 || req_streams.size() > 4) {
        response.set_success(false);
        response.set_message("Stream count must be 1-4");
        return false;
    }

    if (autofocus_controller_) {
        autofocus_controller_->stop();
        autofocus_controller_->invalidate_anchor("media pipeline rebuilt");
    }

    // 1. Build HAL reconfig struct
    std::vector<HalPipelineStreamConfig> hal_streams(req_streams.size());
    for (int i = 0; i < req_streams.size(); i++) {
        const auto& s = req_streams.Get(i);
        auto& hs = hal_streams[i];
        snprintf(hs.stream_id, sizeof(hs.stream_id), "sink%d", i);
        hs.input_width = s.input_width();
        hs.input_height = s.input_height();
        hs.input_framerate = s.input_framerate();
        snprintf(hs.codec, sizeof(hs.codec), "%s", s.codec().c_str());
        hs.encoder_width = s.encoder_width();
        hs.encoder_height = s.encoder_height();
        hs.encoder_framerate = s.encoder_framerate();
        hs.encoder_bitrate = s.encoder_bitrate();
        hs.encoder_gop = s.encoder_gop();
    }

    HalPipelineReconfig hal_reconfig = {};
    hal_reconfig.streams = hal_streams.data();
    hal_reconfig.stream_count = static_cast<uint32_t>(hal_streams.size());

    // 2. Stop consumers
    if (encoded_pub_) encoded_pub_->stop();
    if (rtsp_server_) rtsp_server_->stop();
    if (fd_pub_) fd_pub_->stop();

    auto restart_consumers_after_failure = [&]() {
        HAL_LOG_WARNING("CameraDaemon: restarting consumers after failed pipeline reconfiguration");
        if (rtsp_server_ && config_.rtsp_enabled) {
            rtsp_server_.reset();
            init_rtsp();
            if (encoded_pub_) {
                auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
                encoded_pub_->add_local_listener(
                    [rtsp_weak](const std::string& sn, const HalPacketBuffer* pkt) {
                        if (auto rtsp = rtsp_weak.lock()) rtsp->on_packet(sn, pkt);
                    });
            }
        }
        if (encoded_pub_ && !encoded_pub_->start()) {
            HAL_LOG_ERROR("CameraDaemon: failed to restart EncodedPublisher after pipeline reconfiguration failure");
        }
        if (fd_pub_ && !fd_pub_->start()) {
            HAL_LOG_ERROR("CameraDaemon: failed to restart FdPublisher after pipeline reconfiguration failure");
        }
        if (autofocus_controller_) {
            autofocus_controller_->update_video_context(video_source_->video_ctx());
            autofocus_controller_->start();
        }
    };

    // Release op_mu_ before HAL reconfigure_pipeline — it calls
    // stop_pipeline/start_pipeline which waits for GStreamer callbacks to drain.
    // Those callbacks hold priv->mutex and call output_fn_ which takes op_mu_ (read).
    // Holding op_mu_ (write) here causes AB-BA deadlock.
    lock.unlock();

    // 3. Reconfigure pipeline (~2s)
    auto start_time = std::chrono::steady_clock::now();
    int ret = media_ops->reconfigure_pipeline(media_ctx_, &hal_reconfig);
    auto end_time = std::chrono::steady_clock::now();
    uint32_t interrupt_ms = static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time).count());

    if (ret < 0) {
        HAL_LOG_ERROR("CameraDaemon: reconfigure_pipeline failed: %d", ret);
        response.set_success(false);
        response.set_message("Pipeline reconfiguration failed: " + std::to_string(ret));
        response.set_interrupt_ms(interrupt_ms);
        restart_consumers_after_failure();
        return false;
    }

    // 4. Re-discover stream config from new pipeline
    lock.lock();
    {
        void* video_list = nullptr;
        uint32_t video_count = 0;
        ret = media_ops->get_video_list(media_ctx_, &video_list, &video_count);
        if (ret >= 0 && video_list && video_count > 0) {
            // Update video_source_ contexts (old ones were freed by build_contexts)
            video_source_->init_from_context(static_cast<void**>(video_list), video_count);

            // Rebuild config_.streams to match new pipeline
            config_.streams.clear();
            video_name_map_.clear();
            void** vlist = static_cast<void**>(video_list);
            for (uint32_t i = 0; i < video_count; i++) {
                auto* vc = static_cast<HalVideoContext*>(vlist[i]);
                StreamCfg sc;
                sc.name = vc->video_name;
                sc.width = vc->config.width;
                sc.height = vc->config.height;
                sc.fps = vc->config.framerate;
                config_.streams.push_back(sc);

                // Map pipeline id to config name
                std::string display_name;
                switch (i) {
                    case 0: display_name = "main"; break;
                    case 1: display_name = "sub"; break;
                    case 2: display_name = "third"; break;
                    default: display_name = "stream" + std::to_string(i); break;
                }
                video_name_map_[sc.name] = display_name;
            }

            // Rebind frame callbacks — init_from_context cleared old slots.
            bind_video_source_callbacks();

            // Re-subscribe streams via HAL for the new contexts
            for (auto& slot : video_source_->streams()) {
                video_source_->start_stream(slot.name);
            }
        }
    }

    {
        void* codec_list = nullptr;
        uint32_t codec_count = 0;
        ret = media_ops->get_codec_list(media_ctx_, &codec_list, &codec_count);
        if (ret >= 0 && codec_list && codec_count > 0) {
            config_.encoders.clear();
            encoder_name_map_.clear();
            void** clist = static_cast<void**>(codec_list);
            for (uint32_t i = 0; i < codec_count; i++) {
                auto* cc = static_cast<HalCodecContext*>(clist[i]);
                std::string pipeline_name(cc->codec_name);

                std::string display_name;
                switch (i) {
                    case 0: display_name = "main"; break;
                    case 1: display_name = "sub"; break;
                    case 2: display_name = "third"; break;
                    default: display_name = "stream" + std::to_string(i); break;
                }

                EncoderCfg ec;
                ec.stream_name = display_name;
                ec.width = cc->config.width;
                ec.height = cc->config.height;
                ec.fps = cc->config.framerate;
                ec.bitrate = cc->config.bitrate;
                ec.gop = cc->config.intra_pic_rate;
                ec.codec = (cc->config.packet_type == HAL_PACKET_TYPE_H265) ? "h265" : "h264";
                config_.encoders.push_back(ec);

                encoder_name_map_[pipeline_name] = display_name;
            }
        }
    }

    // Release lock before resync — destroy_all()/unsubscribe can wait for
    // encoder callbacks that try to acquire op_mu_ (read).
    lock.unlock();

    resync_encoders_from_media_pipeline();
    restore_image_config_if_cached();
    reapply_osd_config_after_pipeline_rebuild("pipeline reconfigure");

    lock.lock();

    // 5. Restart consumers
    if (rtsp_server_ && config_.rtsp_enabled) {
        rtsp_server_.reset();
        init_rtsp();
        if (encoded_pub_) {
            auto rtsp_weak = std::weak_ptr<RtspServer>(rtsp_server_);
            encoded_pub_->add_local_listener(
                [rtsp_weak](const std::string& sn, const HalPacketBuffer* pkt) {
                    if (auto rtsp = rtsp_weak.lock()) rtsp->on_packet(sn, pkt);
                });
        }
    }

    if (encoded_pub_) encoded_pub_->start();
    if (fd_pub_) fd_pub_->start();

    if (autofocus_controller_) {
        autofocus_controller_->update_video_context(video_source_->video_ctx());
        autofocus_controller_->start();
    }

    HAL_LOG_INFO("CameraDaemon: Pipeline reconfigured (%u streams, interrupt: %ums)",
                 req_streams.size(), interrupt_ms);

    // Report applied streams back to caller for YAML persistence.
    // Use user-visible names (main/sub/third) to match YAML stream_name.
    for (const auto& ec : config_.encoders) {
        auto* applied = response.add_applied_streams();
        applied->set_stream_id(ec.stream_name);
        applied->set_encoder_width(ec.width);
        applied->set_encoder_height(ec.height);
        applied->set_encoder_framerate(ec.fps);
        applied->set_encoder_bitrate(ec.bitrate);
        applied->set_encoder_gop(ec.gop);
        applied->set_codec(ec.codec);
    }

    response.set_success(true);
    response.set_message("Pipeline reconfigured successfully");
    response.set_interrupt_ms(interrupt_ms);
    return true;
}
#endif

void CameraDaemon::shutdown() {
    HAL_LOG_INFO("CameraDaemon: Shutting down...");

#ifdef HAS_GRPC
    stop_grpc_server();
#endif

    // 1. Stop watchdog (no buffer dependency)
    if (watchdog_) {
        watchdog_->stop();
    }

    // 2. Stop AI overlay subscriber (Event Bus consumer, no buffer dependency)
    if (ai_overlay_) {
        ai_overlay_->stop();
    }

    // 2b. Stop audio service
    if (audio_service_) {
        audio_service_.reset();
    }

    // 2c. Stop DPM worker (HAL inference sessions + FrameRouter subscriber).
    //     Must precede HAL unload (dlclose) and pipeline teardown — the worker
    //     retains ManagedFrames from the router and holds HAL inference sessions.
    stop_dpm_worker();

    // 3. Stop all buffer consumers BEFORE stopping the pipeline.
    //    Resize buffer pools inside Hailo media library reference-count output
    //    buffers.  If we stop the pipeline first (which destroys resize pools)
    //    while consumers (encoders, FD) still hold references, the resize
    //    destructor times out waiting for buffers and then crashes (SIGSEGV)
    //    during forced cleanup.

    // 3a. Stop encoded publisher (consumes encoder packets)
    if (encoded_pub_) {
        encoded_pub_->stop();
    }

    // 3b. Stop RTSP server (consumes encoded packets)
    if (rtsp_server_) {
        rtsp_server_->stop();
    }

    // 3c. Destroy encoders (release resize output buffers via from_media path)
    if (encoder_mgr_) {
        encoder_mgr_->destroy_all();
    }

    // 3d. Stop FD publisher (releases DMA-BUF references)
    if (fd_pub_) {
        fd_pub_->stop();
    }

    // 3e. Stop FrameRouter dispatch thread (drains pending frames)
    if (frame_router_) {
        frame_router_->stop();
    }

    // 4. Stop data source (pipeline stop — safe now: all buffer consumers released)
    auto* media_ops = hal_loader_ ? hal_loader_->media() : nullptr;
    if (media_ops && media_ctx_) {
        // v2 media pipeline mode: unified stop
        media_ops->stop(media_ctx_);
    } else if (video_source_) {
        video_source_->stop_all();
    }

    // 5. Deinit video (FROM_MEDIA contexts skip HAL deinit)
    if (video_source_) {
        video_source_->deinit();
    }

    // 6. Deinit media pipeline (resize buffer pools destroyed — no outstanding refs)
    if (media_ops && media_ctx_) {
        media_ops->deinit(media_ctx_);
        media_ctx_ = nullptr;
    }

    // 7. Unload HAL
    if (hal_loader_) {
        hal_loader_->unload();
    }

    HAL_LOG_INFO("CameraDaemon: Shutdown complete");
}
