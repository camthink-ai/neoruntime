/**
 * @file lens_image_probe.h
 * @brief Image-sharpness differential probe: motorized vs fixed lens.
 *
 * After the boot iris probe files a unit under the no-iris group
 * (af0832 excluded), this probe separates a motorized lens (FG2009) from a
 * fixed-focus lens using the image sensor as the feedback channel: command
 * a short focus jog and watch the ISP AF statistics. A real motor answers
 * with a sharpness dip that returns to baseline when the jog retracts; a
 * motorless lens leaves the statistics flat — but only a textured, stable
 * scene can prove that, so low-texture/unstable scenes stay inconclusive
 * and the caller keeps the (safe) motorized identity.
 */

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

extern "C" {
#include "hal_isp.h"
}

class FrameRouter;
class LensController;
class AutofocusController;

struct LensImageProbeConfig {
    bool enabled = true;
    int steps = 250;                // focus jog, curve steps each way
    int frames = 5;                 // frames per measurement point
    int settle_ms = 400;            // mechanical settle after a jog
    int pps = 600;
    int ready_timeout_ms = 120000;  // lens parked + AF idle + stats readable
    int move_timeout_ms = 15000;
    int frame_wait_timeout_ms = 900;
    uint32_t texture_floor = 3000;  // raw AF sum; below = inconclusive
    double motor_ratio = 0.25;      // dip depth that proves a motor
    double flat_ratio = 0.08;       // flat band (verdict gate + stability gate)
    double return_ratio = 0.15;     // return-to-baseline info (refine trigger)
    double luma_guard_ratio = 0.20; // AE/scene-shift rejection between points
    std::string stream_name = "main";
};

enum class LensImageProbeResult {
    Motorized,     // statistics tracked the jog — a focus motor answered
    FixedLens,     // textured stable scene, statistics flat through the jog
    Inconclusive,  // low texture / unstable scene / not ready in time
};

const char* lens_image_probe_result_name(LensImageProbeResult result);

/**
 * Runs the focus-jog differential probe. Blocking: waits internally for
 * lens readiness, autofocus idle and readable AF statistics (up to
 * cfg.ready_timeout_ms), then takes the autofocus operation lock for the
 * jog window (~3 s). Holds that lock so user/AF motion requests are busy-
 * rejected instead of interleaving with the measurement.
 *
 * return_dev_out, when non-null, receives |S2-S0|/S0 — how far sharpness
 * recovered after the return jog. Open-loop lenses approach from the
 * opposite direction, so gear hysteresis routinely leaves 20-40% behind
 * even though the motor returned to the same step count; the caller can
 * use it to decide whether a refinement pass is warranted.
 */
LensImageProbeResult run_lens_image_probe(HalIspOps* isp_ops, void* video_ctx,
                                          FrameRouter* frame_router,
                                          LensController* lens,
                                          AutofocusController* autofocus,
                                          const LensImageProbeConfig& cfg,
                                          const std::atomic<bool>* stop_flag = nullptr,
                                          double* return_dev_out = nullptr);
