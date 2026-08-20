/**
 * @file lens_hal_service.cpp
 * @brief LensHAL gRPC service implementation
 *
 * Wraps libhal-lens-bridge.so (loaded via dlopen) and exposes every lens
 * HAL operation as a gRPC RPC.  All bridge calls are serialized by mu_.
 */

#ifdef HAS_GRPC

#include "../include/lens_hal_service.h"
#include "lens_hal.grpc.pb.h"

#include <dlfcn.h>
#include <atomic>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>

extern "C" {
    #include "hal_common.h"
    #include "hal_log.h"
    #include "peripheral/devices/hal_lens.h"
}

/* ── Lens state C struct layout (24 bytes, matches Go bridge) ────────── */

#pragma pack(push, 1)
struct BridgeLensState {
    uint8_t  iris_state;     // 0
    uint8_t  zoom_state;     // 1
    uint8_t  focus_state;    // 2
    uint8_t  zoom_rz_done;   // 3  (bool)
    uint8_t  focus_rz_done;  // 4  (bool)
    uint8_t  _pad1[3];       // 5-7 (3 bytes padding to align next int32_t)
    int32_t  zoom_pos;       // 8-11
    int32_t  focus_pos;      // 12-15
    // remaining bytes unused (pad to 24 if needed, but HalIOLensState is 16 bytes)
    uint8_t  _pad2[8];       // 16-23
};
#pragma pack(pop)

static_assert(sizeof(BridgeLensState) == 24, "BridgeLensState must be 24 bytes");

/* ── Motor state constants ───────────────────────────────────────────── */

static constexpr uint8_t MOTOR_STATE_STOPPED    = 1;
static constexpr uint8_t MOTOR_STATE_ERROR      = 4;

/* ── LensHalServiceImpl ─────────────────────────────────────────────── */

class LensHalServiceImpl final : public aipc::lens::LensHAL::Service,
                                 public LensController {
public:
    using Config = LensHalConfig;

    explicit LensHalServiceImpl(const Config& cfg)
        : cfg_(cfg), fg2009_(cfg.lens_model == "fg2009"),
          fg2009_params_(cfg.fg2009) {
        if (fg2009_ && fg2009_params_.ram_steps <= 0) {
            // Hand-built configs (tests/tools) may skip the yaml defaults.
            hal_lens_fg2009_params_init_defaults(&fg2009_params_);
        }
        load_bridge();
    }

    ~LensHalServiceImpl() override {
        if (dl_handle_) {
            dlclose(dl_handle_);
            dl_handle_ = nullptr;
        }
    }

    bool begin_autofocus_operation() override {
        std::lock_guard<std::mutex> lock(mu_);
        bool expected = false;
        return af_operation_active_.compare_exchange_strong(expected, true);
    }

    void end_autofocus_operation() override {
        af_operation_active_.store(false);
    }

    bool autofocus_operation_active() const override {
        return af_operation_active_.load();
    }

    bool initialized() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return initialized_;
    }

    bool af0832_bootstrapped() const override {
        std::lock_guard<std::mutex> lock(mu_);
        return af0832_bootstrapped_;
    }

    int state_get(LensControllerState* state) override {
        if (!state) return HAL_ERR_INVALID_ARG;
        std::lock_guard<std::mutex> lock(mu_);
        if (!initialized_ || !sym_.lens_state_get) return HAL_ERR_NOT_INITIALIZED;
        BridgeLensState raw{};
        const int ret = sym_.lens_state_get(1, &raw);
        if (ret != HAL_OK) return ret;
        state->iris_state = raw.iris_state;
        state->zoom_state = raw.zoom_state;
        state->focus_state = raw.focus_state;
        state->zoom_rz_done = raw.zoom_rz_done != 0;
        state->focus_rz_done = raw.focus_rz_done != 0;
        state->zoom_pos = raw.zoom_pos;
        state->focus_pos = raw.focus_pos;
        if (fg2009_) {
            // Overlay the dead-reckoned model: MCU counters are diagnostics.
            state->zoom_pos = fg2009_state_.zoom_curve;
            state->focus_pos = fg2009_state_.focus_curve;
            state->zoom_rz_done = fg2009_state_.anchored;
            state->focus_rz_done = fg2009_state_.anchored;
        }
        return HAL_OK;
    }

    int zoom_abs_wait(int pps, int32_t position, uint32_t timeout_ms) override {
        int ret = HAL_ERR_NOT_INITIALIZED;
        bool event_waited = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            // Match auto_af_test timing when the AF0832 helper is available:
            // its completion event avoids the coarse motor-state poll interval.
            if (initialized_ && fg2009_) {
                ret = fg2009_zoom_abs_locked(pps, position);
                event_waited = true;  // fire-and-forget; poll below for stop
            } else if (initialized_ && af0832_created_ && af0832_bootstrapped_ &&
                sym_.af0832_zoom_abs) {
                event_waited = true;
                ret = sym_.af0832_zoom_abs(
                    static_cast<uint16_t>(pps), position);
            } else if (initialized_ && sym_.zoom_abs) {
                ret = sym_.zoom_abs(1, pps, position);
            }
        }
        if (ret != HAL_OK) return ret;
        if (event_waited && !fg2009_) return HAL_OK;
        return wait_motor_stopped(true, timeout_ms) ? HAL_OK : HAL_ERR_TIMEOUT;
    }

    int focus_abs_wait(int pps, int32_t position, uint32_t timeout_ms) override {
        int ret = HAL_ERR_NOT_INITIALIZED;
        bool event_waited = false;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (initialized_ && fg2009_) {
                ret = fg2009_focus_abs_locked(pps, position);
                event_waited = true;
            } else if (initialized_ && af0832_created_ && af0832_bootstrapped_ &&
                sym_.af0832_focus_abs) {
                event_waited = true;
                ret = sym_.af0832_focus_abs(
                    static_cast<uint16_t>(pps), position);
            } else if (initialized_ && sym_.focus_abs) {
                ret = sym_.focus_abs(1, pps, position);
            }
        }
        if (ret != HAL_OK) return ret;
        if (event_waited && !fg2009_) return HAL_OK;
        return wait_motor_stopped(false, timeout_ms) ? HAL_OK : HAL_ERR_TIMEOUT;
    }

    int zoom_focus_abs_wait(int zoom_pps, int32_t zoom_position,
                            int focus_pps, int32_t focus_position,
                            uint32_t timeout_ms) override {
        decltype(sym_.af0832_zf_sync_abs) sync_abs = nullptr;
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (fg2009_) {
                // Only the AF follow path uses synchronized dual-axis moves,
                // and autofocus is disabled on FG2009.
                return HAL_ERR_NOT_SUPPORTED;
            }
            if (!initialized_ || !af0832_created_ || !af0832_bootstrapped_) {
                return HAL_ERR_NOT_INITIALIZED;
            }
            sync_abs = sym_.af0832_zf_sync_abs;
        }
        if (!sync_abs) return HAL_ERR_NOT_SUPPORTED;

        /*
         * Do not hold mu_ while waiting. Cancellation must be able to acquire
         * it and send stop commands while a long dual-axis segment is active.
         */
        return sync_abs(static_cast<uint16_t>(zoom_pps), zoom_position,
                        static_cast<uint16_t>(focus_pps), focus_position,
                        timeout_ms);
    }

    int stop_all(uint32_t timeout_ms) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (sym_.zoom_stop) sym_.zoom_stop(1);
            if (sym_.focus_stop) sym_.focus_stop(1);
        }
        const bool zoom_ok = wait_motor_stopped(true, timeout_ms);
        const bool focus_ok = wait_motor_stopped(false, timeout_ms);
        return zoom_ok && focus_ok ? HAL_OK : HAL_ERR_TIMEOUT;
    }

    int calc_targets(float zoom_ratio, float focus_distance_m,
                     int32_t* zoom_target, int32_t* focus_target) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fg2009_) return HAL_ERR_NOT_SUPPORTED;
        if (!sym_.af0832_calc_targets) return HAL_ERR_NOT_SUPPORTED;
        return sym_.af0832_calc_targets(zoom_ratio, focus_distance_m,
                                        zoom_target, focus_target);
    }

    int estimate_distance(float zoom_ratio, int32_t focus_pos,
                          float* distance_m) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fg2009_) return HAL_ERR_NOT_SUPPORTED;
        if (!sym_.af0832_estimate_distance) return HAL_ERR_NOT_SUPPORTED;
        return sym_.af0832_estimate_distance(zoom_ratio, focus_pos, distance_m);
    }

    float pos_to_ratio(int32_t zoom_pos) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fg2009_) {
            // Ratio comes from the dead-reckoned model, not the AF0832 table.
            return hal_lens_fg2009_steps_to_ratio(fg2009_state_.zoom_curve);
        }
        return sym_.af0832_pos_to_ratio ? sym_.af0832_pos_to_ratio(zoom_pos) : 1.0f;
    }

    /* ── Lifecycle RPCs ─────────────────────────────────────────────── */

    grpc::Status Init(grpc::ServerContext* /*ctx*/,
                      const aipc::lens::Empty* /*req*/,
                      aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        if (reject_if_af_active(resp, "init")) return grpc::Status::OK;

        if (!bridge_loaded_) {
            fill_status(resp, -1, "bridge library not loaded");
            return grpc::Status::OK;
        }

        // io_init
        int ret = sym_.io_init(cfg_.serial_device.c_str(),
                               cfg_.baud_rate, cfg_.timeout_ms);
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: io_init failed: %d", ret);
            fill_status(resp, ret, "io_init failed");
            return grpc::Status::OK;
        }

        // lens_init with retry (mirrors Go code)
        ret = sym_.lens_init(1);
        if (ret != 0) {
            HAL_LOG_WARNING("LensHAL: lens_init failed (%d), retrying...", ret);
            sym_.lens_deinit(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ret = sym_.lens_init(1);
            if (ret != 0) {
                HAL_LOG_ERROR("LensHAL: lens_init retry failed: %d", ret);
                fill_status(resp, ret, "lens_init failed after retry");
                return grpc::Status::OK;
            }
        }

        // lens_config
        ret = sym_.lens_config(1, 0);
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: lens_config failed: %d", ret);
            fill_status(resp, ret, "lens_config failed");
            return grpc::Status::OK;
        }

        if (fg2009_) {
            // Open-loop lens: re-select the profile (RAM-only on the MCU) and
            // bootstrap the dead-reckoned position model. No MCU limits.
            const int fret = finish_init_fg2009_locked();
            if (fret != HAL_OK) {
                fill_status(resp, fret, "fg2009 init failed");
                return grpc::Status::OK;
            }
            fill_status(resp, 0, "ok");
            return grpc::Status::OK;
        }

        // Set limits
        if (sym_.zoom_limit_set) {
            sym_.zoom_limit_set(1, cfg_.zoom_min, cfg_.zoom_max);
        }
        if (sym_.focus_limit_set) {
            sym_.focus_limit_set(1, cfg_.focus_min, cfg_.focus_max);
        }

        initialized_ = true;
        HAL_LOG_INFO("LensHAL: initialized (dev=%s baud=%u)",
                     cfg_.serial_device.c_str(), cfg_.baud_rate);
        fill_status(resp, 0, "ok");
        return grpc::Status::OK;
    }

    grpc::Status ReInit(grpc::ServerContext* /*ctx*/,
                        const aipc::lens::Empty* /*req*/,
                        aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        if (reject_if_af_active(resp, "reinit")) return grpc::Status::OK;

        if (!bridge_loaded_) {
            fill_status(resp, -1, "bridge library not loaded");
            return grpc::Status::OK;
        }

        // If never initialized (e.g. camera-daemon restarted), run io_init first
        if (!initialized_) {
            int io_ret = sym_.io_init(cfg_.serial_device.c_str(),
                                      cfg_.baud_rate, cfg_.timeout_ms);
            if (io_ret != 0) {
                HAL_LOG_ERROR("LensHAL: reinit io_init failed: %d", io_ret);
                fill_status(resp, io_ret, "io_init failed on reinit");
                return grpc::Status::OK;
            }
        }

        int ret = sym_.lens_init(1);
        if (ret != 0) {
            HAL_LOG_WARNING("LensHAL: reinit lens_init failed (%d), retrying...", ret);
            sym_.lens_deinit(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ret = sym_.lens_init(1);
        }
        if (ret != 0) {
            fill_status(resp, ret, "lens_init failed on reinit");
            return grpc::Status::OK;
        }

        ret = sym_.lens_config(1, 0);
        if (ret != 0) {
            fill_status(resp, ret, "lens_config failed on reinit");
            return grpc::Status::OK;
        }

        if (fg2009_) {
            const int fret = finish_init_fg2009_locked();
            if (fret != HAL_OK) {
                fill_status(resp, fret, "fg2009 reinit failed");
                return grpc::Status::OK;
            }
            fill_status(resp, 0, "ok");
            return grpc::Status::OK;
        }

        if (sym_.zoom_limit_set) sym_.zoom_limit_set(1, cfg_.zoom_min, cfg_.zoom_max);
        if (sym_.focus_limit_set) sym_.focus_limit_set(1, cfg_.focus_min, cfg_.focus_max);

        initialized_ = true;
        HAL_LOG_INFO("LensHAL: re-initialized");
        fill_status(resp, 0, "ok");
        return grpc::Status::OK;
    }

    grpc::Status Shutdown(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::Empty* /*req*/,
                          aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        if (reject_if_af_active(resp, "shutdown")) return grpc::Status::OK;

        if (af0832_created_ && sym_.af0832_destroy) {
            sym_.af0832_destroy();
            af0832_created_ = false;
        }

        int ret = 0;
        if (sym_.lens_deinit) ret = sym_.lens_deinit(1);
        if (sym_.io_deinit) sym_.io_deinit(1);

        initialized_ = false;
        HAL_LOG_INFO("LensHAL: shutdown");
        fill_status(resp, ret, ret == 0 ? "ok" : "lens_deinit failed");
        return grpc::Status::OK;
    }

    /* ── State RPCs ─────────────────────────────────────────────────── */

    grpc::Status StateGet(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::Empty* /*req*/,
                          aipc::lens::LensState* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        BridgeLensState raw{};
        int ret = sym_.lens_state_get(1, &raw);
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: lens_state_get failed: %d", ret);
            return grpc::Status(grpc::StatusCode::INTERNAL,
                                "lens_state_get failed: " + std::to_string(ret));
        }

        resp->set_iris_state(raw.iris_state);
        resp->set_zoom_state(raw.zoom_state);
        resp->set_focus_state(raw.focus_state);
        resp->set_zoom_rz_done(raw.zoom_rz_done != 0);
        resp->set_focus_rz_done(raw.focus_rz_done != 0);
        resp->set_zoom_pos(raw.zoom_pos);
        resp->set_focus_pos(raw.focus_pos);

        if (fg2009_) {
            // MCU position counters carry no optical meaning on FG2009;
            // report the dead-reckoned curve coordinates instead.
            resp->set_zoom_pos(fg2009_state_.zoom_curve);
            resp->set_focus_pos(fg2009_state_.focus_curve);
            resp->set_zoom_rz_done(fg2009_state_.anchored);
            resp->set_focus_rz_done(fg2009_state_.anchored);
        }

        // Auto-stop any motor stuck in ERROR state — a stopped motor can
        // recover on the next command, while an ERROR-state motor rejects
        // all commands until power-cycled.
        if (raw.zoom_state == MOTOR_STATE_ERROR && sym_.zoom_stop) {
            HAL_LOG_WARNING("LensHAL: zoom motor in ERROR state, auto-stopping");
            int zs_ret = sym_.zoom_stop(1);
            if (zs_ret != 0) {
                HAL_LOG_WARNING("LensHAL: zoom auto-stop returned %d", zs_ret);
            }
        }
        if (raw.focus_state == MOTOR_STATE_ERROR && sym_.focus_stop) {
            HAL_LOG_WARNING("LensHAL: focus motor in ERROR state, auto-stopping");
            int fs_ret = sym_.focus_stop(1);
            if (fs_ret != 0) {
                HAL_LOG_WARNING("LensHAL: focus auto-stop returned %d", fs_ret);
            }
        }

        return grpc::Status::OK;
    }

    grpc::Status IrisAdcGet(grpc::ServerContext* /*ctx*/,
                            const aipc::lens::Empty* /*req*/,
                            aipc::lens::IrisAdcResponse* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        uint16_t adc = 0;
        int ret = sym_.iris_adc_get(1, &adc);
        fill_hal_status_if_error(ret, "iris_adc_get");
        resp->set_adc(adc);
        return grpc::Status::OK;
    }

    grpc::Status GetLimits(grpc::ServerContext* /*ctx*/,
                           const aipc::lens::Empty* /*req*/,
                           aipc::lens::LensLimitsResponse* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        if (fg2009_) {
            auto* zoom = resp->mutable_zoom();
            zoom->set_min_pos(0);
            zoom->set_max_pos(HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
            auto* focus = resp->mutable_focus();
            focus->set_min_pos(0);
            focus->set_max_pos(HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
            return grpc::Status::OK;
        }

        auto* zoom = resp->mutable_zoom();
        zoom->set_min_pos(cfg_.zoom_min);
        zoom->set_max_pos(cfg_.zoom_max);

        auto* focus = resp->mutable_focus();
        focus->set_min_pos(cfg_.focus_min);
        focus->set_max_pos(cfg_.focus_max);
        return grpc::Status::OK;
    }

    /* ── Zoom RPCs ──────────────────────────────────────────────────── */

    grpc::Status ZoomRun(grpc::ServerContext* /*ctx*/,
                         const aipc::lens::MotorRunRequest* req,
                         aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_run")) return grpc::Status::OK;
        int ret;
        if (fg2009_) {
            // MCU run op is not available on FG2009; relative physical steps
            // are the native motion unit.
            ret = fg2009_zoom_rel_locked(req->pps(), req->steps());
        } else {
            ret = sym_.zoom_run(1, req->pps(), req->steps());
        }
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_run failed");
        return grpc::Status::OK;
    }

    grpc::Status ZoomAbs(grpc::ServerContext* /*ctx*/,
                         const aipc::lens::MotorAbsRequest* req,
                         aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_abs")) return grpc::Status::OK;
        int ret;
        if (fg2009_) {
            // Virtual absolute: target in curve coordinates, executed as a
            // relative physical move by the dead-reckoned model.
            ret = fg2009_zoom_abs_locked(req->pps(), req->position());
        } else {
            ret = sym_.zoom_abs(1, req->pps(), req->position());
        }
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_abs failed");
        return grpc::Status::OK;
    }

    grpc::Status ZoomStop(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::Empty* /*req*/,
                          aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_stop")) return grpc::Status::OK;
        int ret = sym_.zoom_stop(1);
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_stop failed");
        return grpc::Status::OK;
    }

    grpc::Status ZoomResetZero(grpc::ServerContext* /*ctx*/,
                               const aipc::lens::Empty* /*req*/,
                               aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_rz")) return grpc::Status::OK;
        if (fg2009_) {
            fill_status(resp, HAL_ERR_NOT_SUPPORTED,
                        "zoom_rz not supported on lens fg2009");
            return grpc::Status::OK;
        }
        int ret = sym_.zoom_rz(1);
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_rz failed");
        return grpc::Status::OK;
    }

    grpc::Status ZoomLimitSet(grpc::ServerContext* /*ctx*/,
                              const aipc::lens::LimitSetRequest* req,
                              aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_limit_set")) return grpc::Status::OK;
        if (fg2009_) {
            // Travel is fixed by the lens profile; no MCU soft limits.
            fill_status(resp, HAL_ERR_NOT_SUPPORTED,
                        "zoom limits fixed by lens profile fg2009");
            return grpc::Status::OK;
        }
        int ret = sym_.zoom_limit_set(1, req->min_pos(), req->max_pos());
        if (ret == 0) {
            cfg_.zoom_min = req->min_pos();
            cfg_.zoom_max = req->max_pos();
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_limit_set failed");
        return grpc::Status::OK;
    }

    grpc::Status WaitZoomStopped(grpc::ServerContext* /*ctx*/,
                                 const aipc::lens::WaitRequest* req,
                                 aipc::lens::HalStatus* resp) override {
        bool ok = wait_motor_stopped(/*is_zoom=*/true, req->timeout_ms());
        fill_status(resp, ok ? 0 : -1, ok ? "ok" : "zoom wait timeout");
        return grpc::Status::OK;
    }

    grpc::Status WaitZoomRzDone(grpc::ServerContext* /*ctx*/,
                                 const aipc::lens::WaitRequest* req,
                                 aipc::lens::HalStatus* resp) override {
        bool ok = wait_rz_done(/*is_zoom=*/true, req->timeout_ms());
        fill_status(resp, ok ? 0 : -1, ok ? "ok" : "zoom rz wait timeout");
        return grpc::Status::OK;
    }

    /* ── Focus RPCs ─────────────────────────────────────────────────── */

    grpc::Status FocusRun(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::MotorRunRequest* req,
                          aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_run")) return grpc::Status::OK;
        int ret;
        if (fg2009_) {
            ret = fg2009_focus_rel_locked(req->pps(), req->steps());
        } else {
            ret = sym_.focus_run(1, req->pps(), req->steps());
        }
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_run failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusAbs(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::MotorAbsRequest* req,
                          aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_abs")) return grpc::Status::OK;
        int ret;
        if (fg2009_) {
            ret = fg2009_focus_abs_locked(req->pps(), req->position());
        } else {
            ret = sym_.focus_abs(1, req->pps(), req->position());
        }
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_abs failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusStop(grpc::ServerContext* /*ctx*/,
                           const aipc::lens::Empty* /*req*/,
                           aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_stop")) return grpc::Status::OK;
        int ret = sym_.focus_stop(1);
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_stop failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusResetZero(grpc::ServerContext* /*ctx*/,
                                const aipc::lens::Empty* /*req*/,
                                aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_rz")) return grpc::Status::OK;
        if (fg2009_) {
            fill_status(resp, HAL_ERR_NOT_SUPPORTED,
                        "focus_rz not supported on lens fg2009");
            return grpc::Status::OK;
        }
        int ret = sym_.focus_rz(1);
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_rz failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusLimitSet(grpc::ServerContext* /*ctx*/,
                               const aipc::lens::LimitSetRequest* req,
                               aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_limit_set")) return grpc::Status::OK;
        if (fg2009_) {
            fill_status(resp, HAL_ERR_NOT_SUPPORTED,
                        "focus limits fixed by lens profile fg2009");
            return grpc::Status::OK;
        }
        int ret = sym_.focus_limit_set(1, req->min_pos(), req->max_pos());
        if (ret == 0) {
            cfg_.focus_min = req->min_pos();
            cfg_.focus_max = req->max_pos();
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_limit_set failed");
        return grpc::Status::OK;
    }

    grpc::Status WaitFocusStopped(grpc::ServerContext* /*ctx*/,
                                  const aipc::lens::WaitRequest* req,
                                  aipc::lens::HalStatus* resp) override {
        bool ok = wait_motor_stopped(/*is_zoom=*/false, req->timeout_ms());
        fill_status(resp, ok ? 0 : -1, ok ? "ok" : "focus wait timeout");
        return grpc::Status::OK;
    }

    grpc::Status WaitFocusRzDone(grpc::ServerContext* /*ctx*/,
                                 const aipc::lens::WaitRequest* req,
                                 aipc::lens::HalStatus* resp) override {
        bool ok = wait_rz_done(/*is_zoom=*/false, req->timeout_ms());
        fill_status(resp, ok ? 0 : -1, ok ? "ok" : "focus rz wait timeout");
        return grpc::Status::OK;
    }

    /* ── Iris RPCs ──────────────────────────────────────────────────── */

    grpc::Status IrisRun(grpc::ServerContext* /*ctx*/,
                         const aipc::lens::MotorRunRequest* req,
                         aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        int ret = sym_.iris_run(1, req->pps(), req->steps());
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "iris_run failed");
        return grpc::Status::OK;
    }

    grpc::Status IrisStop(grpc::ServerContext* /*ctx*/,
                          const aipc::lens::Empty* /*req*/,
                          aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        int ret = sym_.iris_stop(1);
        fill_status(resp, ret, ret == 0 ? "ok" : "iris_stop failed");
        return grpc::Status::OK;
    }

    grpc::Status IrisTargetSet(grpc::ServerContext* /*ctx*/,
                               const aipc::lens::IrisTargetRequest* req,
                               aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        int ret = sym_.iris_target_set(1, static_cast<int>(req->target()));
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "iris_target_set failed");
        return grpc::Status::OK;
    }

    /* ── AF0832 RPCs ────────────────────────────────────────────────── */

    grpc::Status AF0832Bootstrap(grpc::ServerContext* /*ctx*/,
                                 const aipc::lens::Empty* /*req*/,
                                 aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "af0832_bootstrap")) return grpc::Status::OK;
        if (reject_if_fg2009(resp, "af0832_bootstrap")) return grpc::Status::OK;
        ensure_af0832_created();
        int ret = sym_.af0832_bootstrap ? sym_.af0832_bootstrap() : -1;
        if (ret == HAL_OK) af0832_bootstrapped_ = true;
        fill_status(resp, ret, ret == 0 ? "ok" : "af0832_bootstrap failed");
        return grpc::Status::OK;
    }

    grpc::Status AF0832MarkBootstrapped(grpc::ServerContext* /*ctx*/,
                                        const aipc::lens::Empty* /*req*/,
                                        aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "af0832_mark_bootstrapped")) return grpc::Status::OK;
        if (reject_if_fg2009(resp, "af0832_mark_bootstrapped")) return grpc::Status::OK;
        ensure_af0832_created();
        if (sym_.af0832_mark_bootstrapped) {
            sym_.af0832_mark_bootstrapped();
            af0832_bootstrapped_ = true;
        }
        fill_status(resp, 0, "ok");
        return grpc::Status::OK;
    }

    grpc::Status AF0832ForceResetZero(grpc::ServerContext* /*ctx*/,
                                      const aipc::lens::Empty* /*req*/,
                                      aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "af0832_force_reset_zero")) return grpc::Status::OK;
        if (reject_if_fg2009(resp, "af0832_force_reset_zero")) return grpc::Status::OK;
        ensure_af0832_created();
        int ret = sym_.af0832_force_reset_zero ? sym_.af0832_force_reset_zero() : -1;
        fill_status(resp, ret, ret == 0 ? "ok" : "af0832_force_reset_zero failed");
        return grpc::Status::OK;
    }

    grpc::Status AF0832GotoRatioDistance(grpc::ServerContext* /*ctx*/,
                                        const aipc::lens::AF0832GotoRequest* req,
                                        aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "af0832_goto")) return grpc::Status::OK;
        if (reject_if_fg2009(resp, "af0832_goto")) return grpc::Status::OK;
        ensure_af0832_created();
        int ret = sym_.af0832_goto
                  ? sym_.af0832_goto(req->zoom_ratio(), req->focus_distance_m())
                  : -1;
        fill_status(resp, ret, ret == 0 ? "ok" : "af0832_goto failed");
        return grpc::Status::OK;
    }

    grpc::Status AF0832PosToRatio(grpc::ServerContext* /*ctx*/,
                                  const aipc::lens::AF0832PosToRatioRequest* req,
                                  aipc::lens::AF0832PosToRatioResponse* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (fg2009_) {
            // The Go layer reports zoom_ratio from this conversion; the input
            // is the dead-reckoned curve coordinate StateGet overlays.
            resp->set_ratio(hal_lens_fg2009_steps_to_ratio(req->hal_zoom_pos()));
            return grpc::Status::OK;
        }
        if (!sym_.af0832_pos_to_ratio) {
            return grpc::Status(grpc::StatusCode::UNIMPLEMENTED,
                                "af0832_pos_to_ratio not available");
        }
        float ratio = sym_.af0832_pos_to_ratio(req->hal_zoom_pos());
        resp->set_ratio(ratio);
        return grpc::Status::OK;
    }

    grpc::Status IsAF0832Bootstrapped(grpc::ServerContext* /*ctx*/,
                                      const aipc::lens::Empty* /*req*/,
                                      aipc::lens::AF0832BootstrappedResponse* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        resp->set_bootstrapped(fg2009_ ? false : af0832_bootstrapped_);
        return grpc::Status::OK;
    }

    /* ── Lens profile & FG2009 helper RPCs ──────────────────────────── */

    grpc::Status ProfileGet(grpc::ServerContext* /*ctx*/,
                            const aipc::lens::Empty* /*req*/,
                            aipc::lens::LensProfileResponse* resp) override {
        std::lock_guard<std::mutex> lock(mu_);

        resp->set_model(fg2009_ ? "fg2009" : "af0832");

        // Query the MCU so callers can verify the RAM-only selection is
        // still in place (a power glitch reverts it to AF0832).
        HalLensProfileInfo info{};
        if (sym_.profile_get && sym_.profile_get(1, &info) == HAL_OK) {
            const bool mcu_fg2009 = info.model == HAL_LENS_MODEL_FG2009;
            if (mcu_fg2009 != fg2009_) {
                HAL_LOG_ERROR("LensHAL: MCU lens profile mismatch (mcu=%s)",
                              mcu_fg2009 ? "fg2009" : "af0832");
                // Self-heal the common case: re-push and re-read.
                const uint32_t want = fg2009_ ? (uint32_t)HAL_LENS_MODEL_FG2009
                                              : (uint32_t)HAL_LENS_MODEL_AF0832;
                if (sym_.profile_set && sym_.profile_set(1, want) == HAL_OK &&
                    sym_.profile_get(1, &info) == HAL_OK) {
                    const bool healed =
                        (info.model == HAL_LENS_MODEL_FG2009) == fg2009_;
                    resp->set_message(healed
                        ? "profile re-pushed after MCU mismatch"
                        : "profile mismatch persists after re-push");
                } else {
                    resp->set_message("profile mismatch; re-push failed");
                }
            }
            if (fg2009_) {
                resp->set_relative(info.capabilities.sync_relative ||
                                   info.capabilities.relative);
                resp->set_ircut(info.capabilities.ircut);
                // The frozen SoC-side geometry is authoritative everywhere
                // (ratio math, virtual limits); report it, not the MCU's
                // nominal travel, so a stale firmware table cannot skew the
                // web limits. Old bench firmware reports zoom travel 2381
                // while the vendor curve TELE end is 2463.
                if (info.zoom.travel_steps != HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS ||
                    info.focus.travel_steps != HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS) {
                    HAL_LOG_WARNING("LensHAL: MCU fg2009 travel (z=%d f=%d) differs "
                                 "from frozen curve (z=%d f=%d); using frozen",
                                 (int)info.zoom.travel_steps,
                                 (int)info.focus.travel_steps,
                                 (int)HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS,
                                 (int)HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
                }
                resp->set_zoom_travel_steps(HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
                resp->set_focus_travel_steps(HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
                resp->set_max_zoom_ratio(hal_lens_fg2009_max_ratio());
                return grpc::Status::OK;
            }
        }

        if (fg2009_) {
            resp->set_relative(true);
            resp->set_ircut(true);
            resp->set_zoom_travel_steps(HAL_LENS_FG2009_ZOOM_TRAVEL_STEPS);
            resp->set_focus_travel_steps(HAL_LENS_FG2009_FOCUS_TRAVEL_STEPS);
            resp->set_max_zoom_ratio(hal_lens_fg2009_max_ratio());
        } else {
            // Matches the AF0832 zoom-ratio table's tele end.
            resp->set_max_zoom_ratio(2.88f);
        }
        return grpc::Status::OK;
    }

    grpc::Status ZoomGotoRatio(grpc::ServerContext* /*ctx*/,
                               const aipc::lens::ZoomGotoRatioRequest* req,
                               aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_goto_ratio")) return grpc::Status::OK;
        if (reject_if_not_fg2009(resp, "zoom_goto_ratio")) return grpc::Status::OK;
        const int32_t target = hal_lens_fg2009_ratio_to_steps(req->ratio());
        int ret = fg2009_zoom_abs_locked(req->pps(), target);
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_goto_ratio failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusGotoLevel(grpc::ServerContext* /*ctx*/,
                                const aipc::lens::FocusGotoLevelRequest* req,
                                aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_goto_level")) return grpc::Status::OK;
        if (reject_if_not_fg2009(resp, "focus_goto_level")) return grpc::Status::OK;
        const int32_t target = hal_lens_fg2009_focus_level_to_steps(req->level());
        int ret = fg2009_focus_abs_locked(req->pps(), target);
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_goto_level failed");
        return grpc::Status::OK;
    }

    grpc::Status ZoomMoveRel(grpc::ServerContext* /*ctx*/,
                             const aipc::lens::MotorRunRequest* req,
                             aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "zoom_move_rel")) return grpc::Status::OK;
        if (reject_if_not_fg2009(resp, "zoom_move_rel")) return grpc::Status::OK;
        int ret = fg2009_zoom_rel_locked(req->pps(), req->steps());
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "zoom_move_rel failed");
        return grpc::Status::OK;
    }

    grpc::Status FocusMoveRel(grpc::ServerContext* /*ctx*/,
                              const aipc::lens::MotorRunRequest* req,
                              aipc::lens::HalStatus* resp) override {
        std::lock_guard<std::mutex> lock(mu_);
        if (reject_if_af_active(resp, "focus_move_rel")) return grpc::Status::OK;
        if (reject_if_not_fg2009(resp, "focus_move_rel")) return grpc::Status::OK;
        int ret = fg2009_focus_rel_locked(req->pps(), req->steps());
        if (ret != 0) {
            consecutive_errors_++;
            if (consecutive_errors_ >= 3) try_auto_reinit();
        } else {
            consecutive_errors_ = 0;
        }
        fill_status(resp, ret, ret == 0 ? "ok" : "focus_move_rel failed");
        return grpc::Status::OK;
    }

    /* ── Compound RPCs ──────────────────────────────────────────────── */

    grpc::Status StopAndWaitAll(grpc::ServerContext* /*ctx*/,
                                const aipc::lens::WaitRequest* req,
                                aipc::lens::HalStatus* resp) override {
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (reject_if_af_active(resp, "stop_and_wait_all")) return grpc::Status::OK;
            if (sym_.zoom_stop) sym_.zoom_stop(1);
            if (sym_.focus_stop) sym_.focus_stop(1);
        }

        // Brief settle time
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        // Wait for both to stop
        bool zoom_ok = wait_motor_stopped(true, req->timeout_ms());
        bool focus_ok = wait_motor_stopped(false, req->timeout_ms());

        bool ok = zoom_ok && focus_ok;
        fill_status(resp, ok ? 0 : -1, ok ? "ok" : "stop_and_wait timeout");
        return grpc::Status::OK;
    }

private:
    Config          cfg_;
    mutable std::mutex mu_;
    std::atomic<bool> af_operation_active_{false};
    void*           dl_handle_     = nullptr;
    BridgeSymbols   sym_{};
    bool            bridge_loaded_ = false;
    bool            initialized_   = false;
    bool            af0832_created_       = false;
    bool            af0832_bootstrapped_  = false;
    int             consecutive_errors_   = 0;    // reset on any success

    // FG2009 open-loop lens: dead-reckoned position model (curve coords).
    bool                    fg2009_        = false;
    HalLensFg2009Params     fg2009_params_{};
    HalLensFg2009State      fg2009_state_{};

    /* ── dlopen / dlsym ─────────────────────────────────────────────── */

    template<typename Fn>
    Fn resolve(const char* name) {
        void* p = dlsym(dl_handle_, name);
        if (!p) {
            HAL_LOG_WARNING("LensHAL: symbol '%s' not found: %s", name, dlerror());
        }
        return reinterpret_cast<Fn>(p);
    }

    void load_bridge() {
        if (cfg_.library_path.empty()) {
            HAL_LOG_WARNING("LensHAL: no bridge library path configured");
            return;
        }

        dl_handle_ = dlopen(cfg_.library_path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!dl_handle_) {
            HAL_LOG_ERROR("LensHAL: dlopen(%s) failed: %s",
                          cfg_.library_path.c_str(), dlerror());
            return;
        }

        // I/O
        sym_.io_init       = resolve<decltype(sym_.io_init)>("hal_bridge_io_init");
        sym_.io_deinit     = resolve<decltype(sym_.io_deinit)>("hal_bridge_io_deinit");

        // Lens lifecycle
        sym_.lens_init     = resolve<decltype(sym_.lens_init)>("hal_bridge_lens_init");
        sym_.lens_deinit   = resolve<decltype(sym_.lens_deinit)>("hal_bridge_lens_deinit");
        sym_.lens_config   = resolve<decltype(sym_.lens_config)>("hal_bridge_lens_config");
        sym_.lens_state_get = resolve<decltype(sym_.lens_state_get)>("hal_bridge_lens_state_get");

        // Zoom
        sym_.zoom_run      = resolve<decltype(sym_.zoom_run)>("hal_bridge_zoom_run");
        sym_.zoom_abs      = resolve<decltype(sym_.zoom_abs)>("hal_bridge_zoom_abs");
        sym_.zoom_stop     = resolve<decltype(sym_.zoom_stop)>("hal_bridge_zoom_stop");
        sym_.zoom_rz       = resolve<decltype(sym_.zoom_rz)>("hal_bridge_zoom_rz");
        sym_.zoom_limit_set = resolve<decltype(sym_.zoom_limit_set)>("hal_bridge_zoom_limit_set");

        // Focus
        sym_.focus_run     = resolve<decltype(sym_.focus_run)>("hal_bridge_focus_run");
        sym_.focus_abs     = resolve<decltype(sym_.focus_abs)>("hal_bridge_focus_abs");
        sym_.focus_stop    = resolve<decltype(sym_.focus_stop)>("hal_bridge_focus_stop");
        sym_.focus_rz      = resolve<decltype(sym_.focus_rz)>("hal_bridge_focus_rz");
        sym_.focus_limit_set = resolve<decltype(sym_.focus_limit_set)>("hal_bridge_focus_limit_set");

        // Iris
        sym_.iris_run      = resolve<decltype(sym_.iris_run)>("hal_bridge_iris_run");
        sym_.iris_stop     = resolve<decltype(sym_.iris_stop)>("hal_bridge_iris_stop");
        sym_.iris_target_set = resolve<decltype(sym_.iris_target_set)>("hal_bridge_iris_target_set");
        sym_.iris_adc_get  = resolve<decltype(sym_.iris_adc_get)>("hal_bridge_iris_adc_get");

        // Lens profile & physical relative motion (FG2009)
        sym_.profile_set   = resolve<decltype(sym_.profile_set)>("hal_bridge_profile_set");
        sym_.profile_get   = resolve<decltype(sym_.profile_get)>("hal_bridge_profile_get");
        sym_.zoom_rel      = resolve<decltype(sym_.zoom_rel)>("hal_bridge_zoom_rel");
        sym_.focus_rel     = resolve<decltype(sym_.focus_rel)>("hal_bridge_focus_rel");
        sym_.dual_rel      = resolve<decltype(sym_.dual_rel)>("hal_bridge_dual_rel");

        // AF0832
        sym_.af0832_create = resolve<decltype(sym_.af0832_create)>("hal_bridge_af0832_create");
        sym_.af0832_mark_bootstrapped = resolve<decltype(sym_.af0832_mark_bootstrapped)>("hal_bridge_af0832_mark_bootstrapped");
        sym_.af0832_bootstrap = resolve<decltype(sym_.af0832_bootstrap)>("hal_bridge_af0832_bootstrap");
        sym_.af0832_force_reset_zero = resolve<decltype(sym_.af0832_force_reset_zero)>("hal_bridge_af0832_force_reset_zero");
        sym_.af0832_goto   = resolve<decltype(sym_.af0832_goto)>("hal_bridge_af0832_goto_by_ratio_distance");
        sym_.af0832_zoom_abs = resolve<decltype(sym_.af0832_zoom_abs)>("hal_bridge_af0832_zoom_abs");
        sym_.af0832_focus_abs = resolve<decltype(sym_.af0832_focus_abs)>("hal_bridge_af0832_focus_abs");
        sym_.af0832_zf_sync_abs = resolve<decltype(sym_.af0832_zf_sync_abs)>("hal_bridge_af0832_zf_sync_abs");
        sym_.af0832_pos_to_ratio = resolve<decltype(sym_.af0832_pos_to_ratio)>("hal_bridge_af0832_pos_to_ratio");
        sym_.af0832_calc_targets = resolve<decltype(sym_.af0832_calc_targets)>("hal_bridge_af0832_calc_targets");
        sym_.af0832_estimate_distance = resolve<decltype(sym_.af0832_estimate_distance)>("hal_bridge_af0832_estimate_distance");
        sym_.af0832_destroy = resolve<decltype(sym_.af0832_destroy)>("hal_bridge_af0832_destroy");

        // Validate critical symbols
        if (!sym_.io_init || !sym_.lens_init || !sym_.lens_deinit ||
            !sym_.lens_config || !sym_.lens_state_get) {
            HAL_LOG_ERROR("LensHAL: bridge library missing critical symbols");
            dlclose(dl_handle_);
            dl_handle_ = nullptr;
            return;
        }

        bridge_loaded_ = true;
        HAL_LOG_INFO("LensHAL: bridge library loaded from %s", cfg_.library_path.c_str());
        if (fg2009_) {
            if (!sym_.profile_set || !sym_.zoom_rel || !sym_.focus_rel ||
                !sym_.dual_rel) {
                HAL_LOG_ERROR("LensHAL: bridge missing FG2009 profile/relative symbols");
            }
            HAL_LOG_INFO("LensHAL: FG2009 lens (ram=%d park=(%d,%d) pps=(%u,%u))",
                         fg2009_params_.ram_steps, fg2009_params_.park_zoom_steps,
                         fg2009_params_.park_focus_steps, fg2009_params_.zoom_pps,
                         fg2009_params_.focus_pps);
        }
        HAL_LOG_INFO("LensHAL: AF0832 event-driven motion %s",
                     sym_.af0832_zoom_abs && sym_.af0832_focus_abs
                         ? "available"
                         : "unavailable; using motor-state polling");
        HAL_LOG_INFO("LensHAL: AF0832 synchronized dual-axis motion %s",
                     sym_.af0832_zf_sync_abs ? "available" : "unavailable");
    }

    /* ── FG2009 open-loop helpers (mu_ held by callers) ─────────────── */

    /* Push the FG2009 profile and bootstrap the dead-reckoned model:
     * ram both axes into the TELE/FAR hard stops, anchor the model there,
     * then park back by the bench-calibrated offsets. Sets initialized_. */
    int finish_init_fg2009_locked() {
        initialized_ = false;
        fg2009_state_ = {};

        // The MCU keeps the lens selection in RAM only (AF0832 power-on
        // default), so every (re)init must re-select FG2009 before moving.
        if (!sym_.profile_set) return HAL_ERR_NOT_SUPPORTED;
        int ret = sym_.profile_set(1, (uint32_t)HAL_LENS_MODEL_FG2009);
        if (ret != HAL_OK) {
            HAL_LOG_ERROR("LensHAL: profile_set fg2009 failed: %d", ret);
            return ret;
        }

        const uint16_t zpps = hal_lens_fg2009_clamp_pps(fg2009_params_.zoom_pps);
        const uint16_t fpps = hal_lens_fg2009_clamp_pps(fg2009_params_.focus_pps);
        const int32_t ram = fg2009_params_.ram_steps;
        const uint32_t ram_timeout =
            move_timeout_ms(ram, zpps, fpps);
        const uint32_t park_timeout = move_timeout_ms(
            std::max(fg2009_params_.park_zoom_steps,
                     fg2009_params_.park_focus_steps),
            zpps, fpps);

        // Physical negative = Tele/Far: ram both hard stops to establish
        // a repeatable mechanical origin.
        ret = sym_.dual_rel(1, zpps, -ram, fpps, -ram);
        if (ret != HAL_OK) {
            HAL_LOG_ERROR("LensHAL: fg2009 bootstrap ram failed: %d", ret);
            return ret;
        }
        if (!wait_fg2009_motors_locked(ram_timeout)) {
            HAL_LOG_ERROR("LensHAL: fg2009 bootstrap ram did not stop in %u ms",
                          ram_timeout);
            return HAL_ERR_TIMEOUT;
        }
        hal_lens_fg2009_anchor(&fg2009_state_);

        // Park back to the stored position (positive physical = Wide/Near).
        ret = sym_.dual_rel(1, zpps, fg2009_params_.park_zoom_steps,
                            fpps, fg2009_params_.park_focus_steps);
        if (ret != HAL_OK) {
            HAL_LOG_ERROR("LensHAL: fg2009 bootstrap park failed: %d", ret);
            return ret;
        }
        if (!wait_fg2009_motors_locked(park_timeout)) {
            HAL_LOG_ERROR("LensHAL: fg2009 bootstrap park did not stop in %u ms",
                          park_timeout);
            return HAL_ERR_TIMEOUT;
        }
        hal_lens_fg2009_park(&fg2009_state_, &fg2009_params_);

        initialized_ = true;
        HAL_LOG_INFO("LensHAL: FG2009 bootstrapped (zoom=%d focus=%d curve steps)",
                     fg2009_state_.zoom_curve, fg2009_state_.focus_curve);
        return HAL_OK;
    }

    static uint32_t move_timeout_ms(int32_t steps, uint16_t zoom_pps,
                                    uint16_t focus_pps) {
        const uint16_t pps = std::min(zoom_pps, focus_pps);
        const int64_t travel_ms =
            ((int64_t)(steps < 0 ? -steps : steps) * 1000) / (pps > 0 ? pps : 1);
        return (uint32_t)travel_ms + 3000;  // acceleration + serial margin
    }

    /* Poll both motor states without touching mu_ (caller holds it). */
    bool wait_fg2009_motors_locked(uint32_t timeout_ms) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(
                            timeout_ms > 0 ? timeout_ms : 10000);
        while (std::chrono::steady_clock::now() < deadline) {
            BridgeLensState raw{};
            if (sym_.lens_state_get(1, &raw) != HAL_OK) return false;
            if (raw.zoom_state == MOTOR_STATE_ERROR ||
                raw.focus_state == MOTOR_STATE_ERROR) {
                return false;
            }
            if (raw.zoom_state == MOTOR_STATE_STOPPED &&
                raw.focus_state == MOTOR_STATE_STOPPED) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    /* Virtual absolute move: rel(target - current) in physical steps, then
     * fold the completed move into the model. Fire-and-forget like the
     * AF0832-era ZoomAbs RPC; completion via WaitZoomStopped. */
    int fg2009_zoom_abs_locked(uint32_t pps, int32_t target_curve) {
        if (!initialized_ || !fg2009_state_.anchored || !sym_.zoom_rel)
            return HAL_ERR_NOT_INITIALIZED;
        const int32_t delta =
            hal_lens_fg2009_zoom_physical_delta(&fg2009_state_, target_curve);
        if (delta == 0) return HAL_OK;
        const int ret = sym_.zoom_rel(1, hal_lens_fg2009_clamp_pps((uint16_t)pps),
                                      delta);
        if (ret != HAL_OK) return ret;
        hal_lens_fg2009_apply_physical(&fg2009_state_, delta, 0);
        return HAL_OK;
    }

    int fg2009_focus_abs_locked(uint32_t pps, int32_t target_curve) {
        if (!initialized_ || !fg2009_state_.anchored || !sym_.focus_rel)
            return HAL_ERR_NOT_INITIALIZED;
        const int32_t delta =
            hal_lens_fg2009_focus_physical_delta(&fg2009_state_, target_curve);
        if (delta == 0) return HAL_OK;
        const int ret = sym_.focus_rel(1, hal_lens_fg2009_clamp_pps((uint16_t)pps),
                                       delta);
        if (ret != HAL_OK) return ret;
        hal_lens_fg2009_apply_physical(&fg2009_state_, 0, delta);
        return HAL_OK;
    }

    /* Physical relative jog, clamped so the model stays inside travel. */
    int fg2009_zoom_rel_locked(uint32_t pps, int32_t steps) {
        if (!initialized_ || !fg2009_state_.anchored || !sym_.zoom_rel)
            return HAL_ERR_NOT_INITIALIZED;
        const int32_t target = hal_lens_fg2009_clamp_zoom_curve(
            fg2009_state_.zoom_curve - steps);
        const int32_t clamped = fg2009_state_.zoom_curve - target;
        if (clamped == 0) return HAL_OK;
        const int ret = sym_.zoom_rel(1, hal_lens_fg2009_clamp_pps((uint16_t)pps),
                                      clamped);
        if (ret != HAL_OK) return ret;
        hal_lens_fg2009_apply_physical(&fg2009_state_, clamped, 0);
        return HAL_OK;
    }

    int fg2009_focus_rel_locked(uint32_t pps, int32_t steps) {
        if (!initialized_ || !fg2009_state_.anchored || !sym_.focus_rel)
            return HAL_ERR_NOT_INITIALIZED;
        const int32_t target = hal_lens_fg2009_clamp_focus_curve(
            fg2009_state_.focus_curve - steps);
        const int32_t clamped = fg2009_state_.focus_curve - target;
        if (clamped == 0) return HAL_OK;
        const int ret = sym_.focus_rel(1, hal_lens_fg2009_clamp_pps((uint16_t)pps),
                                       clamped);
        if (ret != HAL_OK) return ret;
        hal_lens_fg2009_apply_physical(&fg2009_state_, 0, clamped);
        return HAL_OK;
    }

    /* ── Wait helpers ───────────────────────────────────────────────── */

    bool wait_motor_stopped(bool is_zoom, uint32_t timeout_ms) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 10000);

        while (std::chrono::steady_clock::now() < deadline) {
            BridgeLensState raw{};
            int ret = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ret = sym_.lens_state_get(1, &raw);
            }
            if (ret != 0) return false;

            uint8_t state = is_zoom ? raw.zoom_state : raw.focus_state;
            if (state == MOTOR_STATE_STOPPED) return true;
            if (state == MOTOR_STATE_ERROR) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    bool wait_rz_done(bool is_zoom, uint32_t timeout_ms) {
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::milliseconds(timeout_ms > 0 ? timeout_ms : 10000);

        while (std::chrono::steady_clock::now() < deadline) {
            BridgeLensState raw{};
            int ret = 0;
            {
                std::lock_guard<std::mutex> lock(mu_);
                ret = sym_.lens_state_get(1, &raw);
            }
            if (ret != 0) return false;

            bool done = is_zoom ? (raw.zoom_rz_done != 0) : (raw.focus_rz_done != 0);
            if (done) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return false;
    }

    /* ── AF0832 lazy init ───────────────────────────────────────────── */

    void ensure_af0832_created() {
        if (af0832_created_ || !sym_.af0832_create) return;
        int ret = sym_.af0832_create(cfg_.zoom_min, cfg_.zoom_max,
                                      cfg_.focus_min, cfg_.focus_max,
                                      1200);  // default_pps
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: af0832_create failed: %d", ret);
            return;
        }
        af0832_created_ = true;
        HAL_LOG_INFO("LensHAL: AF0832 created (zoom=[%d,%d] focus=[%d,%d])",
                     cfg_.zoom_min, cfg_.zoom_max, cfg_.focus_min, cfg_.focus_max);
    }

    // Called after N consecutive errors to reset the HAL-to-MCU link
    // without requiring an external ReInit RPC.  Must be called with mu_ held.
    void try_auto_reinit() {
        HAL_LOG_WARNING("LensHAL: %d consecutive errors, triggering auto-reinit",
                     consecutive_errors_);

        // Stop motors to clear any in-flight commands
        if (sym_.zoom_stop)  sym_.zoom_stop(1);
        if (sym_.focus_stop) sym_.focus_stop(1);

        // Deinit + reinit lens (same pattern as Init RPC)
        if (sym_.lens_deinit) sym_.lens_deinit(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));

        int ret = sym_.lens_init(1);
        if (ret != 0) {
            // One retry
            HAL_LOG_WARNING("LensHAL: auto-reinit lens_init failed (%d), retrying...", ret);
            if (sym_.lens_deinit) sym_.lens_deinit(1);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            ret = sym_.lens_init(1);
        }
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: auto-reinit lens_init failed after retry: %d", ret);
            initialized_ = false;
            consecutive_errors_ = 0;  // reset to avoid tight reinit loops
            return;
        }

        ret = sym_.lens_config(1, 0);
        if (ret != 0) {
            HAL_LOG_ERROR("LensHAL: auto-reinit lens_config failed: %d", ret);
            initialized_ = false;
            consecutive_errors_ = 0;  // reset to avoid tight reinit loops
            return;
        }

        if (fg2009_) {
            // Re-push the RAM-only profile and re-anchor the position model.
            if (finish_init_fg2009_locked() != HAL_OK) {
                initialized_ = false;
                consecutive_errors_ = 0;
                return;
            }
            consecutive_errors_ = 0;
            HAL_LOG_INFO("LensHAL: auto-reinit succeeded (fg2009)");
            return;
        }

        // Restore limits
        if (sym_.zoom_limit_set)  sym_.zoom_limit_set(1, cfg_.zoom_min, cfg_.zoom_max);
        if (sym_.focus_limit_set) sym_.focus_limit_set(1, cfg_.focus_min, cfg_.focus_max);

        initialized_ = true;
        consecutive_errors_ = 0;
        HAL_LOG_INFO("LensHAL: auto-reinit succeeded");
    }

    /* ── Status helpers ─────────────────────────────────────────────── */

    static void fill_status(aipc::lens::HalStatus* s, int code, const char* msg) {
        s->set_ok(code == 0);
        s->set_hal_code(code);
        s->set_message(msg);
    }

    bool reject_if_af_active(aipc::lens::HalStatus* status, const char* operation) const {
        if (!af_operation_active_.load()) return false;
        const std::string message = std::string(operation) + ": autofocus operation active";
        fill_status(status, HAL_ERR_INVALID_STATE, message.c_str());
        return true;
    }

    bool reject_if_fg2009(aipc::lens::HalStatus* status, const char* operation) const {
        if (!fg2009_) return false;
        const std::string message =
            std::string(operation) + ": not supported on lens fg2009";
        fill_status(status, HAL_ERR_NOT_SUPPORTED, message.c_str());
        return true;
    }

    /* FG2009-only RPCs take the inverse guard: they are meaningless on a
     * closed-loop AF0832 lens. */
    bool reject_if_not_fg2009(aipc::lens::HalStatus* status, const char* operation) const {
        if (fg2009_) return false;
        const std::string message =
            std::string(operation) + ": requires lens fg2009";
        fill_status(status, HAL_ERR_NOT_SUPPORTED, message.c_str());
        return true;
    }

    static void fill_hal_status_if_error(int code, const char* op) {
        if (code != 0) {
            HAL_LOG_WARNING("LensHAL: %s returned %d", op, code);
        }
    }
};

/* ── Factory ────────────────────────────────────────────────────────────── */

LensHalServiceBundle CreateLensHalService(const LensHalConfig& cfg) {
    LensHalServiceBundle bundle;
    auto service = std::make_unique<LensHalServiceImpl>(cfg);
    bundle.controller = service.get();
    bundle.service = std::move(service);
    return bundle;
}

#endif  // HAS_GRPC
