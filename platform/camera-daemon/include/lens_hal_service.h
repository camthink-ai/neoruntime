/**
 * @file lens_hal_service.h
 * @brief LensHAL gRPC service - wraps libhal-lens-bridge.so via dlopen/dlsym
 *
 * Exposes lens control operations (zoom, focus, iris, AF0832) over gRPC.
 * All bridge calls are serialized through mu_ to guarantee thread safety.
 */

#pragma once

#ifdef HAS_GRPC

#include <cstdint>
#include <mutex>
#include <string>
#include <memory>
#include <grpcpp/grpcpp.h>

#include "lens_controller.h"

extern "C" {
#include "peripheral/devices/hal_lens_fg2009.h"
}

// Forward-declare generated proto types to avoid including heavy headers here.
namespace aipc { namespace lens {
class Empty;
class HalStatus;
class LensState;
class LensLimitsResponse;
class IrisAdcResponse;
class MotorRunRequest;
class MotorAbsRequest;
class LimitSetRequest;
class WaitRequest;
class IrisTargetRequest;
class AF0832GotoRequest;
class AF0832PosToRatioRequest;
class AF0832PosToRatioResponse;
class AF0832BootstrappedResponse;
class LensProfileResponse;
class ZoomGotoRatioRequest;
class FocusGotoLevelRequest;
}}

// Actual base class comes from the generated header; include only in the .cpp.
namespace aipc { namespace lens { class LensHAL; } }

/* ── Configuration ─────────────────────────────────────────────────────── */

struct LensHalConfig {
    std::string library_path;                       // path to libhal-lens-bridge.so
    std::string serial_device  = "/dev/ttyS0";
    uint32_t    baud_rate      = 921600;
    uint32_t    timeout_ms     = 1000;
    int32_t     zoom_min       = -3236;
    int32_t     zoom_max       = 760;
    int32_t     focus_min      = -844;
    int32_t     focus_max      = 592;
    // Factory lens model ("af0832" | "fg2009"), from product.yaml.
    std::string lens_model     = "af0832";
    // FG2009 open-loop bootstrap geometry (camera-daemon.yaml lens.fg2009.*).
    HalLensFg2009Params fg2009 = {};
};

/* ── Bridge symbol table ───────────────────────────────────────────────── */

struct BridgeSymbols {
    // I/O
    int  (*io_init)(const char* dev, uint32_t baud, uint32_t timeout) = nullptr;
    int  (*io_deinit)(int handle)                                      = nullptr;

    // Lens lifecycle
    int  (*lens_init)(int handle)                                      = nullptr;
    int  (*lens_deinit)(int handle)                                    = nullptr;
    int  (*lens_config)(int handle, int mode)                          = nullptr;
    int  (*lens_state_get)(int handle, void* state)                    = nullptr;

    // Zoom
    int  (*zoom_run)(int handle, int pps, int steps)                   = nullptr;
    int  (*zoom_abs)(int handle, int pps, int position)                = nullptr;
    int  (*zoom_stop)(int handle)                                      = nullptr;
    int  (*zoom_rz)(int handle)                                        = nullptr;
    int  (*zoom_limit_set)(int handle, int min_pos, int max_pos)       = nullptr;

    // Focus
    int  (*focus_run)(int handle, int pps, int steps)                  = nullptr;
    int  (*focus_abs)(int handle, int pps, int position)               = nullptr;
    int  (*focus_stop)(int handle)                                     = nullptr;
    int  (*focus_rz)(int handle)                                       = nullptr;
    int  (*focus_limit_set)(int handle, int min_pos, int max_pos)      = nullptr;

    // Iris
    int  (*iris_run)(int handle, int pps, int steps)                   = nullptr;
    int  (*iris_stop)(int handle)                                      = nullptr;
    int  (*iris_target_set)(int handle, int target)                    = nullptr;
    int  (*iris_adc_get)(int handle, void* adc)                        = nullptr;

    // Lens profile & physical relative motion (FG2009)
    int  (*profile_set)(int handle, uint32_t model)                    = nullptr;
    int  (*profile_get)(int handle, void* info)                        = nullptr;
    int  (*zoom_rel)(int handle, uint16_t pps, int32_t steps)          = nullptr;
    int  (*focus_rel)(int handle, uint16_t pps, int32_t steps)         = nullptr;
    int  (*dual_rel)(int handle, uint16_t zoom_pps, int32_t zoom_steps,
                     uint16_t focus_pps, int32_t focus_steps)           = nullptr;

    // AF0832
    int   (*af0832_create)(int zoom_min, int zoom_max,
                           int focus_min, int focus_max, int default_pps) = nullptr;
    void  (*af0832_mark_bootstrapped)()                                   = nullptr;
    int   (*af0832_bootstrap)()                                           = nullptr;
    int   (*af0832_force_reset_zero)()                                    = nullptr;
    int   (*af0832_goto)(float ratio, float distance)                     = nullptr;
    int   (*af0832_zoom_abs)(uint16_t pps, int32_t position)              = nullptr;
    int   (*af0832_focus_abs)(uint16_t pps, int32_t position)             = nullptr;
    int   (*af0832_zf_sync_abs)(uint16_t zoom_pps, int32_t zoom_position,
                                uint16_t focus_pps, int32_t focus_position,
                                uint32_t timeout_ms)                        = nullptr;
    float (*af0832_pos_to_ratio)(int pos)                                 = nullptr;  // optional
    int   (*af0832_calc_targets)(float ratio, float distance,
                                 int32_t* zoom, int32_t* focus)            = nullptr;
    int   (*af0832_estimate_distance)(float ratio, int32_t focus,
                                      float* distance)                     = nullptr;
    void  (*af0832_destroy)()                                             = nullptr;
};

/* ── Service implementation ────────────────────────────────────────────── */

// The class inherits from the generated Service base; the full definition is
// in lens_hal_service.cpp where we include the generated header.
// We declare a helper "Impl" class here that camera_daemon.h can forward-declare
// without pulling in generated gRPC headers.

class LensHalServiceImpl;  // defined in .cpp

struct LensHalServiceBundle {
    std::unique_ptr<grpc::Service> service;
    LensController* controller = nullptr;  // owned by service
};

// Factory function defined in lens_hal_service.cpp. The service owns the
// controller implementation; the controller pointer remains valid with it.
LensHalServiceBundle CreateLensHalService(const LensHalConfig& cfg);

#endif  // HAS_GRPC
