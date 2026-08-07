/**
 * @file camera_daemon.h
 * @brief Camera Daemon - Top-level orchestrator
 *
 * Wires together all modules:
 *   HalLoader → VideoSource → FrameRouter → { FdPublisher, EncoderManager }
 *                                          ↑ FrameWatchdog
 *
 * OSD overlays are now encoder-scoped: configured per-encoder via HalCodecOps,
 * applied internally by the encoder before encoding. No standalone OSD HAL.
 *
 * Startup:  load HAL → init video → init encoders (+ OSD)
 *           → start watchdog → register subscribers → start streams
 * Shutdown: stop streams → stop watchdog → destroy subscribers
 *           → destroy encoders → deinit video → unload HAL
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <cstdint>
#include <ctime>
#include <chrono>
#include <optional>

extern "C" {
#include "hal_video.h"
#include "media/hal_isp.h"
#include "media/hal_media.h"
#include "peripheral/devices/hal_sensor.h"
#include "peripheral/hal_mcu.h"
}

#ifdef HAS_GRPC
#include <grpcpp/grpcpp.h>
#include "camera.pb.h"
#include "camera.grpc.pb.h"
class CameraControlServiceImpl;
#endif

class HalLoader;
#include "audio_service.h"
#include "autofocus_controller.h"
class VideoSource;
class FrameRouter;
class FrameWatchdog;
class OsdManager;
class EncoderManager;
class FdPublisher;
class RtspServer;
class EncodedPublisher;
class AiOverlaySubscriber;
class AudioService;
class DpmWorker;
struct AudioCfg;

#ifdef HAS_GRPC
// Proto forward declarations
namespace aipc {
namespace camera {
class OsdConfigRequest;
}
}
#endif

struct AiOverlayConfig;

/* ========== Configuration structures ========== */

struct StreamCfg {
    std::string name;
    uint32_t    width;
    uint32_t    height;
    uint32_t    fps;
    uint32_t    pool_max_buffers;
    uint32_t    max_queue_size;   // 0 = use default
};

struct EncoderCfg {
    std::string stream_name;    // Which stream to encode
    std::string codec;          // "h264" or "h265"
    uint32_t    width;
    uint32_t    height;
    uint32_t    fps;
    uint32_t    bitrate;
    uint32_t    gop;
    bool        cbr;
    bool        enabled = true; // false = skip at startup, params preserved in YAML

    // Direct HAL config pass-through
    std::string config_path;    // Optional: direct encoder JSON config file
    std::string config_json;    // Optional: direct encoder JSON string
    std::string rc_mode;        // "CBR", "CVBR", "VBR" (overrides cbr)
    uint32_t    max_bitrate;    // For VBR (0 = not set)
    uint32_t    qp_min;         // QP lower bound (0 = HAL default)
    uint32_t    qp_max;         // QP upper bound (0 = HAL default)
    uint32_t    output_pool_max_buffers; // Encoder output buffer pool (0 = HAL default)
    uint32_t    max_queue_size; // Encoded packet queue depth (0 = HAL default)

    // OSD config (passed at encoder init time for Hailo blender)
    std::string osd_config_path; // Optional: external JSON file for OSD template
    std::string osd_config_json; // Optional: OSD JSON string
};

struct OsdOverlayCfg {
    std::string type;           // "text", "datetime", "image"
    std::string stream_name;    // Encoder stream to attach to
    float       x, y;
    std::string text;
    std::string format;         // For datetime
    float       font_size;
    uint8_t     r, g, b, a;
};

struct DaemonConfig {
    // HAL libraries
    std::string video_lib;
    std::string codec_lib;

    // Media pipeline (Hailo15 v2: unified media pipeline config)
    std::string media_config_path;
    std::string media_config_json;
    std::string backup_folder_path;  // Profile backup/restore directory

    // Video device
    std::string device_path;
    uint32_t    device_width;
    uint32_t    device_height;
    uint32_t    device_fps;
    uint32_t    device_format;
    std::string video_config_path;  // Optional: pass-through to HalVideoConfig

    // Streams
    std::vector<StreamCfg> streams;

    // Encoders
    std::vector<EncoderCfg> encoders;

    // OSD overlays (applied on encoder, encoder-scoped)
    std::vector<OsdOverlayCfg> osd_overlays;

    // FD Publisher (for trusted Apps with dma_buf permission)
    std::string fd_pub_sock_path;
    uint32_t    fd_pub_max_clients;
    uint32_t    fd_pub_max_outstanding;

    // Watchdog
    uint32_t    watchdog_scan_ms;
    uint32_t    watchdog_timeout_ms;
    uint32_t    watchdog_warn_ms;

    // RTSP
    bool        rtsp_enabled;
    uint16_t    rtsp_port;

    // Encoded stream publisher (for external plugins)
    bool        encoded_pub_enabled;
    std::string encoded_pub_dir;

    // AI overlay (drawn on encoder frames based on inference results)
    bool        ai_overlay_enabled = false;
    std::string ai_overlay_lib;   // ML overlay HAL .so (empty = auto-detect from codec_lib path)
    std::string ai_overlay_event_bus_endpoint = "unix:///run/aipc/event-bus.sock";
    std::string ai_overlay_topic_prefix = "inference/";
    bool        ai_overlay_draw_labels = true;
    bool        ai_overlay_draw_confidence = true;
    bool        ai_overlay_draw_landmarks = true;
    bool        ai_overlay_enable_face_blur = false;
    uint32_t    ai_overlay_box_thickness = 2;
    // Stream mapping: inference_stream_id → display_encoder_stream
    // e.g. "third" → "main" means results from stream_id="third" drawn on "main" encoder.
    // If empty, auto-generated from configured streams (all → first encoder stream).
    std::unordered_map<std::string, std::string> ai_overlay_stream_map;

    // Logging
    std::string log_level;
    std::string log_file;

    // LED/IR-cut HAL (optional: separate library, monolithic HAL includes it)
    std::string led_lib;

    // Audio
    AudioCfg audio;

    // Lens HAL (optional: dlopen bridge library for motor control)
    std::string lens_bridge_lib;
    std::string lens_serial_device = "/dev/ttyS0";
    uint32_t    lens_baud_rate     = 921600;
    uint32_t    lens_timeout_ms    = 1000;
    int32_t     lens_zoom_min      = -3236;
    int32_t     lens_zoom_max      = 760;
    int32_t     lens_focus_min     = -844;
    int32_t     lens_focus_max     = 592;

    AutofocusConfig autofocus;
};

/* ========== Camera Daemon ========== */

class CameraDaemon {
public:
    CameraDaemon();
    ~CameraDaemon();

    CameraDaemon(const CameraDaemon&) = delete;
    CameraDaemon& operator=(const CameraDaemon&) = delete;

    /**
     * @brief Initialize all subsystems
     * @param config Daemon configuration
     * @return true on success
     */
    bool init(const DaemonConfig& config);

    /**
     * @brief Start frame pipeline (blocking until stop() is called)
     */
    void run();

    /**
     * @brief Signal the daemon to stop (thread-safe)
     */
    void stop();

    /**
     * @brief Update ISP settings using HAL v2 ISP ops
     */
    bool update_isp_settings(const aipc::camera::ISPUpdateRequest& request);

    /**
     * @brief Get current ISP configuration
     */
    bool get_isp_config(aipc::camera::ISPConfigResponse& response);

    /**
     * @brief Get current transform configuration (rotation, flip, dewarp, grayscale)
     */
    bool get_transform_config(aipc::camera::TransformConfig& config);

    /**
     * @brief Update transform configuration via HAL_MEDIA_OPS
     */
    bool set_transform_config(const aipc::camera::TransformConfig& config);

    /**
     * @brief Set a single scalar profile field at runtime (platform-owned config knob).
     *
     * Patches one field of the active media-library profile via HAL_MEDIA_OPS
     * .set_config_field (no full teardown; applies in-place like the override path).
     * Only fields in the daemon allow-list are accepted — config_field deliberately
     * does NOT cover fields already owned by a typed RPC (dewarp/bitrate/gop/rotation/
     * ISP), to avoid a two-writer race. On success the value is mirrored to
     * /data/aipc/etc/media_config_fields.json and replayed on boot.
     * @param req   field_path + type + string-encoded value.
     * @param msg   receives a human-readable status/error message.
     * @return true on success.
     */
    bool set_config_field(const aipc::camera::SetConfigFieldRequest& req, std::string* msg);

    /**
     * @brief Read a single scalar profile field via HAL_MEDIA_OPS.get_config_field.
     * @param field_path  dotted field path.
     * @param type        receives the detected value type.
     * @param value       receives the value encoded as a string.
     * @param msg         receives a human-readable status/error message.
     * @return true on success.
     */
    bool get_config_field(const std::string& field_path,
                          aipc::camera::ConfigFieldType& type,
                          std::string& value, std::string* msg);

    /**
     * @brief Get current privacy mask configuration
     */
    bool get_privacy_mask_config(aipc::camera::PrivacyMaskConfig& config);

    /**
     * @brief Update privacy mask configuration via HAL_MEDIA_OPS
     */
    bool set_privacy_mask_config(const aipc::camera::PrivacyMaskConfig& config);

    /**
     * @brief Update encoder configuration (bitrate, fps, gop) - hot reload
     * @param stream_name Stream name ("main", "sub", "third") to update
     * @param bitrate_bps Bitrate in bits per second (0 = no change)
     * @param framerate Framerate in fps (0 = no change)
     * @param gop GOP size (0 = no change)
     * @return true on success
     */
    bool update_encoder_config(const std::string& stream_name, uint32_t bitrate_bps, uint32_t framerate, uint32_t gop);

    /**
     * @brief Enable/disable RTSP server - hot reload
     */
    bool set_rtsp_enabled(bool enabled);

    /**
     * @brief Update AI overlay configuration - hot reload
     */
    bool update_ai_overlay_config(bool enabled, bool draw_labels, bool draw_confidence,
                                   uint32_t box_thickness);

#ifdef HAS_GRPC
    /**
     * @brief Update OSD configuration - hot reload (static overlays: resolution, FPS, datetime)
     * @param request OSD configuration with per-stream overlay settings
     * @return true on success
     */
    bool update_osd_config(const aipc::camera::OsdConfigRequest& request);

    /**
     * @brief Get current OSD configuration from all encoder streams
     * @param response Populated OSD config response
     * @return true on success
     */
    bool get_osd_config(aipc::camera::OsdConfigResponse& response);

    // OSD config persistence — best-effort disk mirror of last_osd_request_ so
    // web-configured overlays survive camera-daemon restart/deploy/OS-upgrade.
    // Mirror file: /data/aipc/etc/osd_config.json (persistent p3; .json is NOT
    // clobbered by deploy.sh, which only rewrites etc/*.yaml). suppress_bake is
    // never persisted (it is an editor edit-mode transient, forced false here).
    // persist_osd_config_locked: caller MUST hold op_mu_ (called from update_osd_config).
    void persist_osd_config_locked(const aipc::camera::OsdConfigRequest& req);
    // load_osd_config: read the mirror at startup; returns false on missing
    // (INFO), unparseable (WARNING + Clear), or empty (no streams) — never aborts init.
    bool load_osd_config(aipc::camera::OsdConfigRequest* req);
    // Reapply the cached web OSD after MediaLibrary recreates encoder/blender
    // objects. Also clears vendor profile defaults on active encoders first.
    bool reapply_osd_config_after_pipeline_rebuild(const char* reason);

    // Privacy-mask/DPM config persistence — best-effort disk mirror of the last
    // config applied via set_privacy_mask_config so web-configured static regions
    // + DPM labels/mode/color survive camera-daemon restart/deploy/OS-upgrade.
    // Mirror file: /data/aipc/etc/privacy_mask.json (persistent p3; .json is NOT
    // clobbered by deploy.sh, which only rewrites etc/*.yaml).
    // persist_privacy_mask_config is NOT "_locked": its caller
    // (set_privacy_mask_config) does not hold op_mu_ (unlike update_osd_config);
    // it only serializes its const& argument, so no lock is required. Both helpers
    // live under #ifdef HAS_GRPC (same as the OSD helpers); the persist call site
    // inside the unguarded set_privacy_mask_config carries its own #ifdef guard.
    void persist_privacy_mask_config(const aipc::camera::PrivacyMaskConfig& req);
    // load_privacy_mask_config: read the mirror at startup; returns false on
    // missing (INFO), unparseable (WARNING + Clear), or empty (no regions and
    // disabled) — never aborts init.
    bool load_privacy_mask_config(aipc::camera::PrivacyMaskConfig* req);

    // Transform config persistence — best-effort disk mirror of the last
    // web-configured rotation/flip/dewarp/grayscale/dis/eis so runtime overrides
    // survive camera-daemon restart/deploy/OS-upgrade (the medialib resets
    // image_config to YAML defaults on every pipeline (re)init).
    // Mirror file: /data/aipc/etc/transform_config.json (persistent p3; .json is
    // NOT clobbered by deploy.sh, which only rewrites etc/*.yaml).
    // persist_transform_config is NOT "_locked": its caller (set_transform_config)
    // never holds op_mu_ at the persist call (op_mu_ is released around the HAL
    // image-config change); it only serializes its const& argument, so no lock is
    // required. Both helpers live under #ifdef HAS_GRPC (same as the OSD/privacy
    // helpers); the persist call site inside set_transform_config carries its own
    // #ifdef guard.
    void persist_transform_config(const aipc::camera::TransformConfig& req);
    // load_transform_config: read the mirror at startup; returns false on
    // missing (INFO), unparseable (WARNING + Clear), or identity (all fields at
    // their defaults — nothing to reapply) — never aborts init.
    bool load_transform_config(aipc::camera::TransformConfig* req);

    // Scalar config-field persistence — best-effort disk mirror of the last
    // web-configured scalar profile knobs (frontend.hailort.use-hailort-service and
    // other allow-listed fields) so they survive camera-daemon restart/deploy/
    // OS-upgrade. Mirror file: /data/aipc/etc/media_config_fields.json (persistent
    // p3; .json is NOT clobbered by deploy.sh, which only rewrites etc/*.yaml).
    // On boot the mirror is replayed via set_config_field so it overrides the HAL
    // profile default — replay-on-boot wins, making HAL's own profile persistence
    // (set_override_persistent_settings) an idempotent fallback and resolving the
    // two-writer ambiguity. persist_config_field is NOT "_locked": its caller
    // (set_config_field) never holds op_mu_ at the persist call; it only (re)writes
    // one map entry, so no lock is required. Both helpers live under #ifdef HAS_GRPC
    // (same as the transform/OSD/privacy helpers); the persist call site inside the
    // unguarded set_config_field carries its own #ifdef guard.
    void persist_config_field(const std::string& field_path,
                              aipc::camera::ConfigFieldType type, const std::string& value);
    // load_config_fields: read the mirror at startup; returns false on missing
    // (INFO) or unparseable (WARNING + Clear) — never aborts init.
    bool load_config_fields(aipc::camera::MediaConfigFields* req);

    // ISP settings persistence — best-effort disk mirror of the full
    // cached_isp_state_ snapshot (manual tuning + exposure + NR/WDR/powerline/
    // AWB) so web-tuned ISP values survive camera-daemon restart/deploy/
    // OS-upgrade. A FULL state (not the delta request) is persisted so a partial
    // update replays the complete ISP state.
    // Mirror file: /data/aipc/etc/isp_config.json (persistent p3; .json is NOT
    // clobbered by deploy.sh, which only rewrites etc/*.yaml).
    // build_isp_request_from_cache: fill an ISPUpdateRequest from
    // cached_isp_state_ (mirrors the get_isp_config mapping); all fields set so
    // a replayed request fires every branch of update_isp_settings.
    void build_isp_request_from_cache(aipc::camera::ISPUpdateRequest* req) const;
    // persist_isp_config: no-arg (reads cached_isp_state_), atomic tmp+rename,
    // best-effort. Guarded at the call site with #ifdef HAS_GRPC (update_isp_settings
    // is compiled unconditionally but the helper lives under HAS_GRPC).
    void persist_isp_config();
    // load_isp_config: read the snapshot at startup; returns false on missing
    // (INFO) or unparseable (WARNING + Clear) — never aborts init.
    bool load_isp_config(aipc::camera::ISPUpdateRequest* req);

    /**
     * @brief Full encoder reconfiguration - requires brief restart (~100ms)
     * @param request Encoder reconfiguration request (resolution/codec change)
     * @param response Response with success status and interruption duration
     * @return true on success
     */
    bool reconfigure_encoder(const aipc::camera::EncoderReconfigRequest& request,
                           aipc::camera::EncoderReconfigResponse& response);
#endif

    // Profile management (FROM_MEDIA mode)
    std::string get_current_profile() const;
    std::vector<std::string> list_profiles() const;
    // Switch the active medialib profile. On failure the daemon rolls back to the
    // previous profile and (when non-null) writes a human-readable cause into *message
    // (e.g. thermal restriction / unknown profile). Returns true only on a clean switch.
    bool switch_profile(const std::string& profile_name, std::string* message = nullptr);

    // Active-profile persistence — best-effort disk mirror of the last
    // successfully-switched profile name so a web profile change survives
    // camera-daemon restart/deploy/OS-upgrade. Mirror file:
    // /data/aipc/etc/profile_config.json (persistent p3; .json is NOT clobbered
    // by deploy.sh, which only rewrites etc/*.yaml). Plain-string JSON (no proto),
    // so these helpers are unconditional (not HAS_GRPC-gated), matching
    // switch_profile/get_current_profile.
    // persist_profile_config: atomic tmp+rename, best-effort (caller's HAL switch
    // already succeeded; only the restart-survival mirror is at stake).
    void persist_profile_config(const std::string& profile_name);
    // load_profile_config: read the mirror at startup; returns false on missing
    // (INFO) or unparseable (WARNING) — never aborts init.
    bool load_profile_config(std::string* profile_name);
    bool backup_profile(const std::string& path);

    // Sensor module information
    bool get_sensor_info(uint32_t sensor_index, HalVideoSensorModuleInfo* info) const;

    // IR-Cut filter control
    bool set_ircut(uint32_t mode);  // 0=day, 1=night
    bool get_ircut(uint32_t& mode);

    bool start_autofocus_one_shot(uint64_t* job_id, std::string* error);
    bool start_autofocus_zoom_follow(float ratio, uint64_t* job_id, std::string* error);
    bool cancel_autofocus(uint64_t job_id, std::string* error);
    void invalidate_autofocus_anchor(const std::string& reason);
    AutofocusStatus get_autofocus_status() const;

    // LED control (white light, IR LED)
    bool set_led_duty(uint32_t led_id, uint32_t duty_percent);
    bool get_led_duty(uint32_t led_id, uint32_t& duty_percent);

    // Device hardware status
    bool get_device_hardware_status(aipc::camera::DeviceHardwareStatus& status);

    // MCU raw request
    int mcu_raw_request(uint16_t cmd, const uint8_t* payload, uint16_t payload_len,
                        uint8_t* response, uint16_t response_size, uint16_t& response_len);

    /** Access HAL loader for peripheral ops (env_ctrl, alarm, rs485). */
    HalLoader* hal_loader() const { return hal_loader_.get(); }
    AudioService* audio_service() const { return audio_service_.get(); }

#ifdef HAS_GRPC
    /**
     * @brief Get real-time status of all encoder streams
     * @param response Proto response to populate with per-stream status
     */
    void get_stream_status(aipc::camera::GetStreamStatusResponse& response);

    /**
     * @brief Dynamically add a new encoder stream
     * @param request Stream parameters (stream_id, width, height, fps, codec, bitrate, gop)
     * @param response Operation result
     */
    void add_stream(const aipc::camera::AddStreamRequest& request,
                    aipc::camera::StreamOperationResponse& response);

    /**
     * @brief Dynamically remove an encoder stream
     * @param stream_name Stream to remove (e.g. "third")
     * @param response Operation result
     */
    void remove_stream(const std::string& stream_name,
                       aipc::camera::StreamOperationResponse& response);
#endif

    // Pipeline reconfiguration (FROM_MEDIA mode, ~2s interruption)
#ifdef HAS_GRPC
    bool reconfigure_pipeline(const aipc::camera::ReconfigurePipelineRequest& request,
                              aipc::camera::ReconfigurePipelineResponse& response);
#endif

private:
    void try_share_lens_mcu_ctx();
    void init_mcu_context();

    DaemonConfig config_;
    std::atomic<bool> running_{false};
    // Latches shutdown requests received while init() is still running.
    // run() must never clear this state, otherwise systemd restart can remain
    // stuck in "deactivating" until TimeoutStopSec/SIGKILL.
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> override_in_progress_{false};
    // Effective encoder feed mode for FROM_MEDIA:
    // true  -> MediaLibrary auto-feed frontend->encoder
    // false -> manual feed via FrameRouter encoder subscriber
    std::atomic<bool> encoder_auto_feed_enabled_{false};
    void*                          media_ctx_ = nullptr;

    // Protects mutable state during hot-reload operations (switch_profile, reconfigure, etc.)
    // Hot-reload methods take unique_lock; high-frequency read paths take shared_lock.
    mutable std::shared_mutex op_mu_;
    // Remains held while op_mu_ is temporarily released around blocking HAL calls.
    // This prevents two ReconfigurePipeline RPCs from entering MediaLibrary concurrently.
    std::mutex pipeline_reconfig_mu_;
    // Serializes the full body of switch_profile() (HAL switch + post-switch frame
    // verify + rollback). op_mu_ is intentionally released through the HAL-call and
    // verify/rollback windows, so without this guard a second concurrent profile
    // switch would race the first against a half-rebuilt pipeline and black-screen.
    // Held for the whole function; RAII covers every return path.
    std::mutex profile_switch_mu_;
    // Cooldown for switch_profile(): start time of the last switch attempt that
    // passed the throttle gate. Bounds the switch rate so the ISP FE/sensor state
    // machine settles between reconfigures (rapid back-to-back switches starve it
    // of priming buffers -- kernel logs "fast toggle called without priming buffer
    // set" -- and wedge the FE into an unrecoverable ERROR). Guarded by
    // profile_switch_mu_ (only touched while holding it).
    std::optional<std::chrono::steady_clock::time_point> last_switch_start_time_;
    // Serializes the full body of set_transform_config(). A rotation (orientation
    // swap) forces a full medialib teardown+rebuild inside the HAL (~14s), during
    // which op_mu_ is released (see set_transform_config); without this guard a
    // second concurrent transform RPC would race the first across that window and
    // wedge the rebuilt pipeline (encoded output pools go undrained -> deadlock).
    // Distinct from profile_switch_mu_ — a concurrent switch_profile + transform
    // remains a pre-existing, independently-guarded race. Held for the whole function.
    std::mutex transform_mu_;

    // FROM_MEDIA stream name mapping: media_pipeline_id → config_name
    // e.g. "sink0" → "main", "sink1" → "sub"
    std::unordered_map<std::string, std::string> video_name_map_;
    std::unordered_map<std::string, std::string> encoder_name_map_;
    bool has_media_pipeline() const {
        return media_ctx_ != nullptr;
    }

    std::unique_ptr<HalLoader>      hal_loader_;
    std::unique_ptr<VideoSource>    video_source_;
    std::unique_ptr<FrameRouter>    frame_router_;
    std::unique_ptr<FrameWatchdog>  watchdog_;
    std::unique_ptr<OsdManager>     osd_mgr_;
    std::unique_ptr<EncoderManager> encoder_mgr_;
    std::unique_ptr<FdPublisher>    fd_pub_;
    std::shared_ptr<RtspServer>     rtsp_server_;
    std::unique_ptr<EncodedPublisher> encoded_pub_;
    std::unique_ptr<AiOverlaySubscriber> ai_overlay_;
    std::unique_ptr<AudioService>        audio_service_;
    // Dynamic Privacy Mask worker. shared_ptr so the RPC thread can swap it
    // out from under the 30fps frontend attach lambda without blocking: the
    // lambda copies the shared_ptr under dpm_mu_ (microseconds) then runs
    // attach on its local copy. start()/stop() (HEF load / session destroy,
    // ~1-2s) happen OUTSIDE the lock so the frontend thread never stalls.
    std::shared_ptr<DpmWorker>           dpm_worker_;
    mutable std::shared_mutex            dpm_mu_;

#ifdef HAS_GRPC
    std::unique_ptr<CameraControlServiceImpl> camera_control_service_;
    std::unique_ptr<grpc::Service> lens_hal_service_;
    LensController* lens_controller_ = nullptr;
    std::unique_ptr<AutofocusController> autofocus_controller_;
    std::unique_ptr<grpc::Server> grpc_server_;
    void start_grpc_server();
    void stop_grpc_server();
#endif

    struct FpsTracker {
        uint64_t frame_count = 0;
        time_t   start_time  = 0;
    };

    std::unordered_map<std::string, FpsTracker> fps_trackers_;
    std::unordered_set<std::string> osd_enabled_streams_;  // streams with OSD configured
#ifdef HAS_GRPC
    // Last OSD config request. Disabled overlays are NOT baked (skipped in the
    // add loops) so they can't bleed through when the medialib doesn't honor
    // set_overlay_enabled(false). This cache keeps them in the GET round-trip
    // (with their full config + enabled=false) so the UI preserves them.
    std::unique_ptr<aipc::camera::OsdConfigRequest> last_osd_request_;
    // Editor edit-mode: when true update_osd_config clears but bakes NO text OR
    // image overlays (clean stream for the HTML proxy layer). Set from each
    // request's suppress_bake; the proxies are the single visible text/image
    // layers while editing. Datetime is intentionally still baked (it has no
    // proxy content — only corner hotspots — so the baked time stays visible).
    bool osd_suppress_bake_ = false;
#endif

    // ISP state cache — stores last applied ISP settings for partial updates and readback
    HalIspImageConfig cached_isp_state_;
    bool isp_state_initialized_ = false;
    void init_isp_cache();  // Initialize cache from HAL or defaults

    // Transform/image config cache. Runtime overrides (rotation/flip/dewarp/grayscale)
    // applied via dynamic_change_image_config() live only in the media context and are
    // lost when the pipeline is rebuilt from the medialib config file (deinit+init on
    // stream re-enable/rollback). We capture the last-applied values and reapply them
    // after every such rebuild so stop/start of a stream no longer resets them.
    HalMediaImageConfig last_image_config_ = {};
    bool have_last_image_config_ = false;
    void restore_image_config_if_cached();  // reapply after deinit+init rebuild

    // init_from_context() clears VideoSource callbacks. Keep every bind path on
    // the same pre-encode overlay route so DPM survives resolution/profile rebuilds.
    void bind_video_source_callbacks();
    void handle_video_frame_for_routing(const std::string& dispatch_name, HalFrameBuffer* frame);

    // DPM worker lifecycle. start loads HEFs + allocates the clean-capture
    // ping-pong input buffers (~1-2s, runs OUTSIDE dpm_mu_); stop drains any
    // in-flight offer_frame() + joins the worker thread + destroys sessions.
    // The worker is NOT a FrameRouter subscriber — the frontend frame lambda
    // calls offer_frame() on the streaming thread with the clean pre-bake main
    // frame. Both start/stop swap the shared_ptr under dpm_mu_ so the frontend
    // lambda never observes a half-built worker and never blocks on HEF load.
    // Returns true when a live worker was swapped in (or was already running
    // with the same labels — #4 dedup); false on preconditions-missing or
    // HEF-load / session-create failure. The caller uses the return value to
    // persist dpm_enabled_ honestly (a failed start must NOT report DPM as
    // enabled, or the UI toggle silently lies).
    bool start_dpm_worker(const aipc::camera::PrivacyMaskConfig& config);
    void stop_dpm_worker();

    // Privacy mask cache — HAL stores shallow pointers to these items and their
    // id/name C strings in its current image config, so both the item array and
    // string backing storage must outlive set_privacy_mask_config().
    mutable std::mutex privacy_mask_mu_;
    std::vector<HalPrivacyMaskItem> cached_pm_items_;
    std::vector<std::string> cached_pm_ids_;
    std::vector<std::string> cached_pm_names_;

    // Dynamic Privacy Mask running state. camera-daemon is the source of truth
    // for these fields (the media library does not persist them); set_ stores
    // the effective values and get_ returns them, so the web UI round-trips
    // toggle/labels/mode/color correctly across refreshes.
    //
    // dpm_render_mode_ is an atomic<int> (not a string) because the frontend
    // frame callback reads it on a GStreamer streaming thread at ~30fps while
    // set_privacy_mask_config writes it on the RPC thread; the string form is
    // reconstructed in get_privacy_mask_config for the API response. dpm_color_
    // is likewise atomic for the same lock-free hot-path read.
    bool dpm_enabled_ = false;
    std::string dpm_labels_;                     // target CSV (may be empty → idle worker, #7)
    std::atomic<int> dpm_render_mode_{0};        // 0=mosaic, 1=blur, 2=overlay
    std::atomic<uint32_t> dpm_color_{0x1F2937};  // 0x00RRGGBB, DPM-only (independent of static)
    // #4 restart dedup: dpm_armed_ is true only while a worker is actually live,
    // dpm_armed_labels_ is the label CSV that live worker was built for. A re-PUT
    // that doesn't change armed-state or labels skips the ~1-2s HEF reload.
    bool dpm_armed_ = false;
    std::string dpm_armed_labels_;

    bool init_hal();
    bool init_media();
    bool init_video();
    bool init_encoders();
    bool init_rtsp();
    bool init_encoded_publisher();
    bool init_ai_overlay();
    /** Re-register EncoderManager from get_codec_list after MediaLibrary reallocates codec contexts. */
    void resync_encoders_from_media_pipeline();

    /**
     * Post-change frame verify: poll the primary encoder stream for fresh encoded
     * packets within budget_ms. Returns true once a frame arrives (seen_first_packet
     * AND ms_since_last_packet < kVerifyFreshMs), false on timeout. If primary_out is
     * non-null it receives the resolved media stream name (empty if none resolvable).
     * Used by switch_profile() to verify both the forward switch and any rollback.
     * Caller must NOT hold op_mu_ (on_packet takes it read) — mirrors the existing
     * verify window. Constants kVerifyPollMs / kVerifyFreshMs live in the .cpp.
     */
    bool verify_primary_stream_frames(uint64_t budget_ms, std::string* primary_out = nullptr);
    void configure_osd();
    void register_subscribers();

    void shutdown();
};
