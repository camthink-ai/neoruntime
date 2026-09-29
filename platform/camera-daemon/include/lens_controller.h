/**
 * @file lens_controller.h
 * @brief In-process lens control surface shared by LensHAL RPCs and autofocus.
 */

#pragma once

#include <cstdint>
#include <functional>

struct LensControllerState {
    uint8_t iris_state = 0;
    uint8_t zoom_state = 0;
    uint8_t focus_state = 0;
    bool zoom_rz_done = false;
    bool focus_rz_done = false;
    int32_t zoom_pos = 0;
    int32_t focus_pos = 0;
};

class LensController {
public:
    virtual ~LensController() = default;

    virtual bool begin_autofocus_operation() = 0;
    virtual void end_autofocus_operation() = 0;
    virtual bool autofocus_operation_active() const = 0;

    // Fixed-lens identity (image-probe verdict): once marked, the unit is
    // known to carry no zoom/focus motors and every motion request is
    // rejected with HAL_ERR_NOT_SUPPORTED. Default no-op keeps tests and
    // tools source-compatible (same contract as the observers below).
    virtual void mark_fixed_lens() {}
    virtual bool fixed_lens() const { return false; }

    virtual bool initialized() const = 0;
    virtual bool af0832_bootstrapped() const = 0;
    virtual int state_get(LensControllerState* state) = 0;
    virtual int zoom_abs_wait(int pps, int32_t position, uint32_t timeout_ms) = 0;
    virtual int focus_abs_wait(int pps, int32_t position, uint32_t timeout_ms) = 0;
    virtual int zoom_focus_abs_wait(int zoom_pps, int32_t zoom_position,
                                    int focus_pps, int32_t focus_position,
                                    uint32_t timeout_ms) = 0;
    virtual int stop_all(uint32_t timeout_ms) = 0;

    virtual int calc_targets(float zoom_ratio, float focus_distance_m,
                             int32_t* zoom_target, int32_t* focus_target) = 0;
    virtual int estimate_distance(float zoom_ratio, int32_t focus_pos,
                                  float* distance_m) = 0;
    virtual float pos_to_ratio(int32_t zoom_pos) = 0;

    // FG2009 only: notified right after every issued zoom move, with the new
    // optical ratio derived from the dead-reckoned model. The default no-op
    // keeps other builders (tests/tools) source-compatible. Called with the
    // lens mutex held: implementations must not call back into the controller.
    virtual void set_zoom_motion_observer(std::function<void(float)> /*obs*/) {}

    // Position-recorder arming hook: fired on every successfully issued user
    // motion (RPC handlers and the wait-methods above), including moves the
    // autofocus controller makes through those methods. Never fired for
    // init/bootstrap parking, so a restore archive cannot be clobbered by the
    // boot park. Same contract as the observer: mutex held, no call-backs.
    virtual void set_motion_listener(std::function<void()> /*listener*/) {}
};
