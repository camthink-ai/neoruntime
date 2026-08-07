/**
 * @file hailo15_lens_impl.cpp
 * @brief hailo15 lens motor primitives (MCU host_link).
 */

#include "../mcu/hailo15_mcu_priv.hpp"

extern "C" {
#include "peripheral/devices/hal_lens.h"
#include "peripheral/hal_mcu.h"
}

#include "common/host_link/host_link_proto.h"

#include <cstring>

static int status_only(void *mcu_ctx, uint16_t cmd, const void *req, uint16_t req_len)
{
    host_link_status_t st{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, cmd,
                                      reinterpret_cast<const uint8_t *>(req), req_len,
                                      reinterpret_cast<uint8_t *>(&st), sizeof(st), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(st)) return HAL_ERR_INVALID_SIZE;
    return hailo15_mcu_map_status(st.status);
}

static int lens_init(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_INIT, nullptr, 0); }
static int lens_deinit(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_DEINIT, nullptr, 0); }

static int lens_config(void *mcu_ctx, HalLensMode mode)
{
    host_link_lens_cfg_t req{.mode = (uint8_t)mode};
    return status_only(mcu_ctx, HOST_LINK_CMD_LENS_CFG, &req, sizeof(req));
}

static int state_get(void *mcu_ctx, HalLensState *out_state)
{
    if (out_state == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_lens_state_t raw{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_LENS_STATE_GET, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&raw), sizeof(raw), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(raw)) return HAL_ERR_INVALID_SIZE;
    out_state->iris_state = raw.iris_state;
    out_state->zoom_state = raw.zoom_state;
    out_state->focus_state = raw.focus_state;
    out_state->zoom_rz_done = (raw.zoom_rz_done != 0);
    out_state->focus_rz_done = (raw.focus_rz_done != 0);
    out_state->zoom_pos = raw.zoom_pos;
    out_state->focus_pos = raw.focus_pos;
    return HAL_OK;
}

static int motion_cmd(void *mcu_ctx, uint16_t cmd, const HalLensMotion *motion)
{
    if (motion == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_lens_motion_t req{.pps = motion->pps, .value = motion->value};
    return status_only(mcu_ctx, cmd, &req, sizeof(req));
}

static int limit_cmd(void *mcu_ctx, uint16_t cmd, const HalLensLimit *limit)
{
    if (limit == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_lens_limit_t req{.min_pos = limit->min_pos, .max_pos = limit->max_pos};
    return status_only(mcu_ctx, cmd, &req, sizeof(req));
}

static int iris_run(void *mcu_ctx, const HalLensMotion *motion) { return motion_cmd(mcu_ctx, HOST_LINK_CMD_LENS_IRIS_RUN, motion); }
static int iris_stop(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_IRIS_STOP, nullptr, 0); }

static int iris_target_set(void *mcu_ctx, uint16_t target)
{
    host_link_lens_iris_tgt_t req{.target = target};
    return status_only(mcu_ctx, HOST_LINK_CMD_LENS_IRIS_TGT_SET, &req, sizeof(req));
}

static int iris_adc_get(void *mcu_ctx, uint16_t *out_adc)
{
    if (out_adc == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_lens_iris_adc_t resp{};
    uint16_t resp_len = 0;
    int ret = HAL_MCU_OPS.raw_request(mcu_ctx, HOST_LINK_CMD_LENS_IRIS_ADC_GET, nullptr, 0,
                                      reinterpret_cast<uint8_t *>(&resp), sizeof(resp), &resp_len);
    if (ret != HAL_OK) return ret;
    if (resp_len != sizeof(resp)) return HAL_ERR_INVALID_SIZE;
    *out_adc = resp.adc;
    return HAL_OK;
}

static int zoom_run(void *mcu_ctx, const HalLensMotion *motion) { return motion_cmd(mcu_ctx, HOST_LINK_CMD_LENS_ZOOM_RUN, motion); }
static int zoom_abs(void *mcu_ctx, const HalLensMotion *motion) { return motion_cmd(mcu_ctx, HOST_LINK_CMD_LENS_ZOOM_ABS, motion); }
static int zoom_stop(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_ZOOM_STOP, nullptr, 0); }
static int zoom_rz(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_ZOOM_RZ, nullptr, 0); }
static int zoom_limit_set(void *mcu_ctx, const HalLensLimit *limit) { return limit_cmd(mcu_ctx, HOST_LINK_CMD_LENS_ZOOM_LIM_SET, limit); }

static int focus_run(void *mcu_ctx, const HalLensMotion *motion) { return motion_cmd(mcu_ctx, HOST_LINK_CMD_LENS_FOCUS_RUN, motion); }
static int focus_abs(void *mcu_ctx, const HalLensMotion *motion) { return motion_cmd(mcu_ctx, HOST_LINK_CMD_LENS_FOCUS_ABS, motion); }
static int focus_stop(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_FOCUS_STOP, nullptr, 0); }
static int focus_rz(void *mcu_ctx) { return status_only(mcu_ctx, HOST_LINK_CMD_LENS_FOCUS_RZ, nullptr, 0); }
static int focus_limit_set(void *mcu_ctx, const HalLensLimit *limit) { return limit_cmd(mcu_ctx, HOST_LINK_CMD_LENS_FOCUS_LIM_SET, limit); }

static int zf_sync_run(void *mcu_ctx, const HalLensZfSync *params)
{
    if (params == nullptr) return HAL_ERR_INVALID_ARG;
    host_link_lens_zf_sync_t req{
        .zm_pps = params->zm_pps,
        .zm_micro_steps = params->zm_micro_steps,
        .fs_pps = params->fs_pps,
        .fs_micro_steps = params->fs_micro_steps,
    };
    return status_only(mcu_ctx, HOST_LINK_CMD_LENS_ZF_SYNC_RUN, &req, sizeof(req));
}

static int subscribe(void *mcu_ctx, HalLensEventCallback cb, void *userdata)
{
    if (cb == nullptr) return HAL_ERR_INVALID_ARG;
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_register_event_cb(ctx, HOST_LINK_CMD_EV_LENS,
                                         [ctx, cb, userdata](uint16_t cmd, const uint8_t *payload, uint16_t len) {
                                             (void)cmd;
                                             if (payload == nullptr || len != sizeof(host_link_lens_evt_t)) {
                                                 return;
                                             }
                                             host_link_lens_evt_t ev{};
                                             memcpy(&ev, payload, sizeof(ev));
                                             cb(ctx, ev.event, ev.result, ev.zoom_pos, ev.focus_pos, userdata);
                                         });
}

static int unsubscribe(void *mcu_ctx)
{
    auto *ctx = static_cast<Hailo15McuCtx *>(mcu_ctx);
    return hailo15_mcu_unregister_all_event_cbs(ctx, HOST_LINK_CMD_EV_LENS);
}

static const char *lens_get_version(void)
{
    return "Hailo15 HAL-LENS 2.0.0";
}

extern "C" {
HalLensOps HAL_LENS_OPS = {
    .lens_init = lens_init,
    .lens_deinit = lens_deinit,
    .lens_config = lens_config,
    .state_get = state_get,
    .iris_run = iris_run,
    .iris_stop = iris_stop,
    .iris_target_set = iris_target_set,
    .iris_adc_get = iris_adc_get,
    .zoom_run = zoom_run,
    .zoom_abs = zoom_abs,
    .zoom_stop = zoom_stop,
    .zoom_rz = zoom_rz,
    .zoom_limit_set = zoom_limit_set,
    .focus_run = focus_run,
    .focus_abs = focus_abs,
    .focus_stop = focus_stop,
    .focus_rz = focus_rz,
    .focus_limit_set = focus_limit_set,
    .zf_sync_run = zf_sync_run,
    .subscribe = subscribe,
    .unsubscribe = unsubscribe,
    .get_version = lens_get_version,
};
}

