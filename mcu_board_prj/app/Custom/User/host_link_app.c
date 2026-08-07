#include "host_link_app.h"
#include "usart.h"
#include "sys_config.h"
#include "rtc.h"
#include "bsp_ctrl.h"
#include "rs485_driver.h"
#include "ms41908m.h"
#include "ota_host.h"
#include <string.h>
#include <stdio.h>

#define HL_EV_TX_DONE  ((EventBits_t)(1u << 0))
#define HL_EV_ERR      ((EventBits_t)(1u << 1))

static host_link_handler_t *g_hl;
static EventGroupHandle_t s_events;
static SemaphoreHandle_t s_tx_mutex;
static TaskHandle_t s_uart_task;
static TaskHandle_t s_poll_task;

static uint8_t s_rx_buf[HOST_LINK_APP_RX_BUF_SIZE];
static uint16_t s_rx_dma_rd;
static volatile uint32_t s_uart2_last_err;

static HAL_StatusTypeDef hl_uart2_restart_rx_dma(void)
{
    (void)HAL_UART_AbortReceive(&huart2);
    s_rx_dma_rd = 0;
    return HAL_UART_Receive_DMA(&huart2, s_rx_buf, (uint16_t)sizeof(s_rx_buf));
}

/** Drain USART2 circular RX DMA using CNDTR (same idea as cmd_console). */
static void hl_uart2_drain_rx_dma(void)
{
    DMA_HandleTypeDef *hdma = huart2.hdmarx;
    uint16_t bufsize = (uint16_t)sizeof(s_rx_buf);
    uint16_t w;

    if (g_hl == NULL || hdma == NULL) {
        return;
    }

    w = (uint16_t)(bufsize - __HAL_DMA_GET_COUNTER(hdma));
    if (w == s_rx_dma_rd) {
        return;
    }

    if (w > s_rx_dma_rd) {
        host_link_feed(g_hl, &s_rx_buf[s_rx_dma_rd], (uint16_t)(w - s_rx_dma_rd));
    } else {
        host_link_feed(g_hl, &s_rx_buf[s_rx_dma_rd], (uint16_t)(bufsize - s_rx_dma_rd));
        if (w > 0u) {
            host_link_feed(g_hl, &s_rx_buf[0], w);
        }
    }
    s_rx_dma_rd = w;
}

static void hl_lens_host_notify(uint32_t event, int result)
{
    host_link_handler_t *hl = g_hl;
    host_link_lens_evt_t pay;
    int r;

    if (hl == NULL) {
        return;
    }

    pay.event = event;
    pay.result = result;
    pay.zoom_pos = ms41908m_get_zoom_position();
    pay.focus_pos = ms41908m_get_focus_position();

    // WIC_LOGD("[host_link] EV_LENS ev=0x%08X result=%d zoom_pos=%d focus_pos=%d", (unsigned)event, result, pay.zoom_pos, pay.focus_pos);
    r = host_link_send_event(hl, HOST_LINK_CMD_EV_LENS, &pay, sizeof(pay));
    // if (r == HOST_LINK_ERR_TIMEOUT) {
    //     /* Host may be absent or not ACKing EVENT in time; keep silent to avoid noisy warnings
    //      * during local shell tests where lens callbacks are still enabled. */
    //     return;
    // }
    if (r != HOST_LINK_OK) {
        WIC_LOGW("[host_link] EV_LENS err=%d evt=%lu ret=%d", r, (unsigned long)event, result);
    }
}

static void hl_rs485_host_notify(const uint8_t *data, uint16_t len, void *user)
{
    host_link_handler_t *hl = (host_link_handler_t *)user;
    int r;

    if (hl == NULL || data == NULL || len == 0u) {
        return;
    }
    r = host_link_send_event(hl, HOST_LINK_CMD_EV_RS485_RX, data, len);
    if (r != HOST_LINK_OK) {
        WIC_LOGW("[host_link] EV_RS485_RX err=%d len=%u", r, (unsigned)len);
    }
}

static void hl_reply_status(host_link_handler_t *h, host_link_frame_t *f, int32_t status)
{
    host_link_status_t s = {.status = status};
    (void)host_link_response(h, f, &s, sizeof(s));
}

static void host_link_dispatch_request(host_link_handler_t *h, host_link_frame_t *f)
{
    const uint8_t *p = f->payload;
    uint16_t len = f->header.len;

    if (ota_host_handle_request(h, f)) {
        return;
    }

    switch ((host_link_cmd_t)f->header.cmd) {
    case HOST_LINK_CMD_PING: {
        uint32_t tick = osKernelGetTickCount();
        (void)host_link_response(h, f, &tick, sizeof(tick));
        break;
    }
    case HOST_LINK_CMD_ECHO:
        if (p != NULL && len > 0u) {
            (void)host_link_response(h, f, p, len);
        } else {
            (void)host_link_response(h, f, NULL, 0);
        }
        break;

    case HOST_LINK_CMD_GET_VERSION: {
        host_link_version_t v;
        int mj = 0, mn = 0, pt = 0;
        memset(&v, 0, sizeof(v));
        (void)sscanf(APP_VERSION, "%d.%d.%d", &mj, &mn, &pt);
        v.major = mj;
        v.minor = mn;
        v.patch = pt;
        (void)host_link_response(h, f, &v, sizeof(v));
        break;
    }

    case HOST_LINK_CMD_RTC_GET: {
        RTC_TimeTypeDef t;
        RTC_DateTypeDef d;
        host_link_rtc_tm_t out;
        if (HAL_RTC_GetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK) {
            hl_reply_status(h, f, SYS_ERR_HAL);
            break;
        }
        (void)HAL_RTC_GetDate(&hrtc, &d, RTC_FORMAT_BIN);
        out.year = d.Year;
        out.month = d.Month;
        out.day = d.Date;
        out.weekday = d.WeekDay;
        out.hour = t.Hours;
        out.minute = t.Minutes;
        out.second = t.Seconds;
        (void)host_link_response(h, f, &out, sizeof(out));
        break;
    }

    case HOST_LINK_CMD_RTC_SET: {
        RTC_TimeTypeDef t;
        RTC_DateTypeDef d;
        const host_link_rtc_tm_t *tm;
        if (p == NULL || len < sizeof(host_link_rtc_tm_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        tm = (const host_link_rtc_tm_t *)p;
        if (tm->month < 1u || tm->month > 12u || tm->day < 1u || tm->day > 31u || tm->weekday < 1u || tm->weekday > 7u ||
            tm->hour > 23u || tm->minute > 59u || tm->second > 59u) {
            hl_reply_status(h, f, SYS_ERR_OUT_OF_RANGE);
            break;
        }
        t.Hours = tm->hour;
        t.Minutes = tm->minute;
        t.Seconds = tm->second;
        t.TimeFormat = RTC_HOURFORMAT12_AM;
        t.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        t.StoreOperation = RTC_STOREOPERATION_RESET;
        d.Year = tm->year;
        d.Month = tm->month;
        d.Date = tm->day;
        d.WeekDay = tm->weekday;
        if (HAL_RTC_SetTime(&hrtc, &t, RTC_FORMAT_BIN) != HAL_OK || HAL_RTC_SetDate(&hrtc, &d, RTC_FORMAT_BIN) != HAL_OK) {
            hl_reply_status(h, f, SYS_ERR_HAL);
            break;
        }
        hl_reply_status(h, f, SYS_OK);
        break;
    }

    case HOST_LINK_CMD_LED_SET: {
        const host_link_led_set_t *ls;
        if (p == NULL || len < sizeof(host_link_led_set_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        ls = (const host_link_led_set_t *)p;
        if (ls->led_id >= (uint8_t)BSP_LED_MAX || ls->duty > 100u) {
            hl_reply_status(h, f, SYS_ERR_OUT_OF_RANGE);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_led_duty((bsp_led_t)ls->led_id, ls->duty));
        break;
    }

    case HOST_LINK_CMD_LED_GET: {
        uint8_t duty;
        if (p == NULL || len < 1u || p[0] >= (uint8_t)BSP_LED_MAX) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        duty = bsp_ctrl_get_led_duty((bsp_led_t)p[0]);
        (void)host_link_response(h, f, &duty, sizeof(duty));
        break;
    }

    case HOST_LINK_CMD_IRCUT_SET: {
        if (p == NULL || len < 1u) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_ir_cut(p[0]));
        break;
    }

    case HOST_LINK_CMD_IRCUT_GET: {
        uint8_t en = bsp_ctrl_get_ir_cut();
        (void)host_link_response(h, f, &en, sizeof(en));
        break;
    }

    case HOST_LINK_CMD_PD_GET: {
        host_link_adc_milli_t o;
        uint16_t mv = bsp_ctrl_get_pd_voltage_mv();
        float lux = bsp_ctrl_convert_pd_to_lux(mv);
        o.mv = mv;
        o.milli = (int32_t)(lux * 1000.0f);
        (void)host_link_response(h, f, &o, sizeof(o));
        break;
    }

    case HOST_LINK_CMD_TEMP_GET: {
        host_link_adc_milli_t o;
        uint16_t mv = bsp_ctrl_get_temp_voltage_mv();
        float c = bsp_ctrl_convert_lmt87_to_celcius(mv);
        o.mv = mv;
        o.milli = (int32_t)(c * 1000.0f);
        (void)host_link_response(h, f, &o, sizeof(o));
        break;
    }

    case HOST_LINK_CMD_FAN_SET: {
        if (p == NULL || len < 1u || p[0] > 100u) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_fan_duty(p[0]));
        break;
    }

    case HOST_LINK_CMD_FAN_GET: {
        uint8_t d = bsp_ctrl_get_fan_duty();
        (void)host_link_response(h, f, &d, sizeof(d));
        break;
    }

    case HOST_LINK_CMD_HEAT_SET: {
        if (p == NULL || len < 1u || p[0] > 100u) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_heat_duty(p[0]));
        break;
    }

    case HOST_LINK_CMD_HEAT_GET: {
        uint8_t d = bsp_ctrl_get_heat_duty();
        (void)host_link_response(h, f, &d, sizeof(d));
        break;
    }

    case HOST_LINK_CMD_RADAR_SET: {
        if (p == NULL || len < 1u) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_radar_en(p[0]));
        break;
    }

    case HOST_LINK_CMD_RADAR_GET: {
        uint8_t e = bsp_ctrl_get_radar_en();
        (void)host_link_response(h, f, &e, sizeof(e));
        break;
    }

    case HOST_LINK_CMD_AOUT_SET: {
        const host_link_ch_enable_t *ce;
        if (p == NULL || len < sizeof(host_link_ch_enable_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        ce = (const host_link_ch_enable_t *)p;
        if (ce->channel >= (uint8_t)BSP_ALARM_OUT_MAX) {
            hl_reply_status(h, f, SYS_ERR_OUT_OF_RANGE);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_alarm_out((bsp_alarm_out_t)ce->channel, ce->enable));
        break;
    }

    case HOST_LINK_CMD_AOUT_GET: {
        if (p == NULL || len < 1u || p[0] >= (uint8_t)BSP_ALARM_OUT_MAX) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        {
            uint8_t e = bsp_ctrl_get_alarm_out((bsp_alarm_out_t)p[0]);
            (void)host_link_response(h, f, &e, sizeof(e));
        }
        break;
    }

    case HOST_LINK_CMD_WOUT_SET: {
        const host_link_ch_enable_t *ce;
        if (p == NULL || len < sizeof(host_link_ch_enable_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        ce = (const host_link_ch_enable_t *)p;
        if (ce->channel >= (uint8_t)BSP_WIEGAND_MAX) {
            hl_reply_status(h, f, SYS_ERR_OUT_OF_RANGE);
            break;
        }
        hl_reply_status(h, f, (int32_t)bsp_ctrl_set_wiegand((bsp_wiegand_t)ce->channel, ce->enable));
        break;
    }

    case HOST_LINK_CMD_WOUT_GET: {
        if (p == NULL || len < 1u || p[0] >= (uint8_t)BSP_WIEGAND_MAX) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        {
            uint8_t e = bsp_ctrl_get_wiegand((bsp_wiegand_t)p[0]);
            (void)host_link_response(h, f, &e, sizeof(e));
        }
        break;
    }

    case HOST_LINK_CMD_AIN_GET: {
        int lv;
        if (p == NULL || len < 1u || p[0] >= (uint8_t)BSP_ALARM_IN_MAX) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        lv = bsp_ctrl_get_alarm_in((bsp_alarm_in_t)p[0]);
        if (lv < 0) {
            hl_reply_status(h, f, (int32_t)lv);
            break;
        }
        {
            uint8_t u = (uint8_t)lv;
            (void)host_link_response(h, f, &u, sizeof(u));
        }
        break;
    }

    case HOST_LINK_CMD_RESET_SOC:
        hl_reply_status(h, f, (int32_t)bsp_ctrl_reset_soc_power());
        break;

    case HOST_LINK_CMD_RS485_INIT: {
        const host_link_rs485_init_t *rcfg;
        char cfg[4] = {0};
        int r;
        if (p == NULL || len < sizeof(host_link_rs485_init_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        rcfg = (const host_link_rs485_init_t *)p;
        cfg[0] = rcfg->config[0];
        cfg[1] = rcfg->config[1];
        cfg[2] = rcfg->config[2];
        r = rs485_driver_init(rcfg->baudrate, cfg);
        if (r == SYS_OK) {
            (void)rs485_driver_set_rx_callback(hl_rs485_host_notify, h);
        }
        hl_reply_status(h, f, r);
        break;
    }

    case HOST_LINK_CMD_RS485_DEINIT:
        (void)rs485_driver_set_rx_callback(NULL, NULL);
        hl_reply_status(h, f, rs485_driver_deinit());
        break;

    case HOST_LINK_CMD_RS485_TX:
        if (p == NULL || len == 0u) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        hl_reply_status(h, f, rs485_driver_send(p, len, RS485_DEFAULT_TIMEOUT_MS));
        break;

    case HOST_LINK_CMD_LENS_INIT:
        ms41908m_set_event_callback(hl_lens_host_notify);
        hl_reply_status(h, f, ms41908m_init());
        break;

    case HOST_LINK_CMD_LENS_DEINIT:
        ms41908m_set_event_callback(NULL);
        ms41908m_deinit();
        hl_reply_status(h, f, SYS_OK);
        break;

    case HOST_LINK_CMD_LENS_CFG: {
        uint8_t mode = 0u;
        int r;
        if (p != NULL && len >= sizeof(host_link_lens_cfg_t)) {
            mode = ((const host_link_lens_cfg_t *)p)->mode;
        }
        if (mode == 0u) {
            r = ms41908m_iris_config(&g_default_iris_config);
            if (r == SYS_OK) {
                r = ms41908m_motor_config(&g_default_motor_config);
            }
            hl_reply_status(h, f, r);
        } else if (mode == 1u) {
            hl_reply_status(h, f, ms41908m_iris_config(&g_default_iris_config));
        } else if (mode == 2u) {
            hl_reply_status(h, f, ms41908m_motor_config(&g_default_motor_config));
        } else {
            hl_reply_status(h, f, SYS_ERR_OUT_OF_RANGE);
        }
        break;
    }

    case HOST_LINK_CMD_LENS_STATE_GET: {
        host_link_lens_state_t s;
        s.iris_state = (uint8_t)ms41908m_iris_get_state();
        s.zoom_state = (uint8_t)ms41908m_get_zoom_state();
        s.focus_state = (uint8_t)ms41908m_get_focus_state();
        s.zoom_rz_done = (uint8_t)ms41908m_zoom_is_reset_zero();
        s.focus_rz_done = (uint8_t)ms41908m_focus_is_reset_zero();
        s.zoom_pos = ms41908m_get_zoom_position();
        s.focus_pos = ms41908m_get_focus_position();
        (void)host_link_response(h, f, &s, sizeof(s));
        break;
    }

    case HOST_LINK_CMD_LENS_IRIS_RUN:
        hl_reply_status(h, f, ms41908m_iris_run());
        break;

    case HOST_LINK_CMD_LENS_IRIS_STOP:
        hl_reply_status(h, f, ms41908m_iris_stop());
        break;

    case HOST_LINK_CMD_LENS_IRIS_TGT_SET: {
        const host_link_lens_iris_tgt_t *req;
        if (p == NULL || len < sizeof(host_link_lens_iris_tgt_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_iris_tgt_t *)p;
        hl_reply_status(h, f, ms41908m_iris_update_target(req->target));
        break;
    }

    case HOST_LINK_CMD_LENS_IRIS_ADC_GET: {
        host_link_lens_iris_adc_t out;
        out.adc = ms41908m_iris_read_adc();
        (void)host_link_response(h, f, &out, sizeof(out));
        break;
    }

    case HOST_LINK_CMD_LENS_ZOOM_RUN: {
        const host_link_lens_motion_t *req;
        if (p == NULL || len < sizeof(host_link_lens_motion_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_motion_t *)p;
        hl_reply_status(h, f, ms41908m_zoom_run(req->pps, req->value));
        break;
    }

    case HOST_LINK_CMD_LENS_ZOOM_ABS: {
        const host_link_lens_motion_t *req;
        if (p == NULL || len < sizeof(host_link_lens_motion_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_motion_t *)p;
        hl_reply_status(h, f, ms41908m_zoom_run_to_position(req->pps, req->value));
        break;
    }

    case HOST_LINK_CMD_LENS_ZOOM_STOP:
        hl_reply_status(h, f, ms41908m_zoom_stop());
        break;

    case HOST_LINK_CMD_LENS_ZOOM_RZ:
        hl_reply_status(h, f, ms41908m_zoom_reset_zero());
        break;

    case HOST_LINK_CMD_LENS_ZOOM_LIM_SET: {
        const host_link_lens_limit_t *req;
        if (p == NULL || len < sizeof(host_link_lens_limit_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_limit_t *)p;
        hl_reply_status(h, f, ms41908m_zoom_set_position_limit(req->min_pos, req->max_pos));
        break;
    }

    case HOST_LINK_CMD_LENS_FOCUS_RUN: {
        const host_link_lens_motion_t *req;
        if (p == NULL || len < sizeof(host_link_lens_motion_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_motion_t *)p;
        hl_reply_status(h, f, ms41908m_focus_run(req->pps, req->value));
        break;
    }

    case HOST_LINK_CMD_LENS_FOCUS_ABS: {
        const host_link_lens_motion_t *req;
        if (p == NULL || len < sizeof(host_link_lens_motion_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_motion_t *)p;
        hl_reply_status(h, f, ms41908m_focus_run_to_position(req->pps, req->value));
        break;
    }

    case HOST_LINK_CMD_LENS_FOCUS_STOP:
        hl_reply_status(h, f, ms41908m_focus_stop());
        break;

    case HOST_LINK_CMD_LENS_FOCUS_RZ:
        hl_reply_status(h, f, ms41908m_focus_reset_zero());
        break;

    case HOST_LINK_CMD_LENS_FOCUS_LIM_SET: {
        const host_link_lens_limit_t *req;
        if (p == NULL || len < sizeof(host_link_lens_limit_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_limit_t *)p;
        hl_reply_status(h, f, ms41908m_focus_set_position_limit(req->min_pos, req->max_pos));
        break;
    }

    case HOST_LINK_CMD_LENS_ZF_SYNC_RUN: {
        const host_link_lens_zf_sync_t *req;
        if (p == NULL || len < sizeof(host_link_lens_zf_sync_t)) {
            hl_reply_status(h, f, SYS_ERR_INVALID_ARG);
            break;
        }
        req = (const host_link_lens_zf_sync_t *)p;
        hl_reply_status(h, f, ms41908m_zf_sync_run(req->zm_pps, req->zm_micro_steps,
                                                     req->fs_pps, req->fs_micro_steps));
        break;
    }

    default:
        hl_reply_status(h, f, SYS_ERR_NOT_SUPPORTED);
        break;
    }
}

/** Runs in @ref bsp_ctrl_task; blocks until host EVENT_ACK (same as other sync host_link sends). */
static void hl_alarm_in_host_notify(bsp_alarm_in_t in, uint8_t level)
{
    host_link_handler_t *hl = g_hl;
    host_link_alarm_in_evt_t pay;

    if (hl == NULL) {
        return;
    }
    pay.channel = (uint8_t)in;
    pay.level = level;
    {
        int r = host_link_send_event(hl, HOST_LINK_CMD_EV_ALARM_IN, &pay, sizeof(pay));
        if (r != HOST_LINK_OK) {
            WIC_LOGW("[host_link] EV_ALARM_IN err=%d ch=%u L=%u", r, (unsigned)in, (unsigned)level);
        }
    }
}

static void host_link_app_notify(void *user_ctx, host_link_handler_t *h, host_link_frame_t *f)
{
    (void)user_ctx;

    if (f->header.type == HOST_LINK_TYPE_REQUEST) {
        host_link_dispatch_request(h, f);
    } else if (f->header.type == HOST_LINK_TYPE_EVENT) {
        (void)host_link_event_ack(h, f);
    }
}

static int hl_uart2_send(void *user_ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms)
{
    EventBits_t ev;
    (void)user_ctx;

    if (s_tx_mutex == NULL || s_events == NULL) {
        return -1;
    }

    if (xSemaphoreTake(s_tx_mutex, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }

    if (HAL_UART_Transmit_DMA(&huart2, (uint8_t *)(uintptr_t)buf, len) != HAL_OK) {
        (void)HAL_UART_AbortTransmit(&huart2);
        (void)xSemaphoreGive(s_tx_mutex);
        return -1;
    }

    ev = xEventGroupWaitBits(s_events, HL_EV_TX_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    if ((ev & HL_EV_TX_DONE) == 0) {
        (void)HAL_UART_AbortTransmit(&huart2);
        (void)xSemaphoreGive(s_tx_mutex);
        return -1;
    }

    (void)xSemaphoreGive(s_tx_mutex);
    return 0;
}

static void host_link_uart_task(void *pv)
{
    HAL_StatusTypeDef st;
    EventBits_t ev;
    (void)pv;

    st = hl_uart2_restart_rx_dma();

    for (;;) {
        ev = xEventGroupWaitBits(s_events, HL_EV_ERR, pdTRUE, pdFALSE, pdMS_TO_TICKS(2));

        if ((ev & HL_EV_ERR) != 0) {
            uint32_t err = s_uart2_last_err;
            s_uart2_last_err = 0u;
            if (err == 0u) {
                WIC_LOGW("[host_link] USART2 spurious error callback (code=0), keep RX running");
                if (huart2.RxState != HAL_UART_STATE_BUSY_RX) {
                    st = hl_uart2_restart_rx_dma();
                }
                continue;
            }
            if ((err & HAL_UART_ERROR_ORE) != 0u) {
                WIC_LOGW("[host_link] USART2 overrun (ORE), quick recover, code=%lu", (unsigned long)err);
            } else {
                WIC_LOGE("[host_link] USART2 error, code=%lu", (unsigned long)err);
            }
            st = hl_uart2_restart_rx_dma();
            continue;
        }

        if (st != HAL_OK) {
            vTaskDelay(pdMS_TO_TICKS(2));
            st = hl_uart2_restart_rx_dma();
            continue;
        }

        if (huart2.RxState == HAL_UART_STATE_BUSY_RX) {
            hl_uart2_drain_rx_dma();
        }
    }
}

static void host_link_poll_task_fn(void *pv)
{
    (void)pv;
    for (;;) {
        if (g_hl != NULL) {
            host_link_poll(g_hl);
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

void host_link_app_on_uart2_rx_event(UART_HandleTypeDef *huart, uint16_t size)
{
    (void)huart;
    (void)size;
    /* Circular DMA RX: host_link_uart_task drains via __HAL_DMA_GET_COUNTER (not ReceiveToIdle / RxEvent). */
}

void host_link_app_on_uart2_tx_done(UART_HandleTypeDef *huart)
{
    BaseType_t hp = pdFALSE;

    if (huart->Instance != USART2 || s_events == NULL) {
        return;
    }

    (void)xEventGroupSetBitsFromISR(s_events, HL_EV_TX_DONE, &hp);
    portYIELD_FROM_ISR(hp);
}

void host_link_app_on_uart2_error(UART_HandleTypeDef *huart)
{
    BaseType_t hp = pdFALSE;

    if (huart->Instance != USART2 || s_events == NULL) {
        return;
    }

    s_uart2_last_err = huart->ErrorCode;
    (void)xEventGroupSetBitsFromISR(s_events, HL_EV_ERR, &hp);
    portYIELD_FROM_ISR(hp);
}

int host_link_app_init(void)
{
    if (g_hl != NULL) {
        return SYS_ERR_INVALID_STATE;
    }

    s_events = xEventGroupCreate();
    if (s_events == NULL) {
        return SYS_ERR_NO_MEM;
    }

    s_tx_mutex = xSemaphoreCreateMutex();
    if (s_tx_mutex == NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }

    g_hl = host_link_init(hl_uart2_send, host_link_app_notify, NULL);
    if (g_hl == NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }

    if (ota_host_init() != SYS_OK) {
        host_link_deinit(g_hl);
        g_hl = NULL;
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_INVALID_STATE;
    }

    if (xTaskCreate(host_link_uart_task, HOST_LINK_APP_TASK_UART_NAME, HOST_LINK_APP_TASK_UART_STACK, NULL,
                    HOST_LINK_APP_TASK_UART_PRIORITY, &s_uart_task) != pdPASS) {
        host_link_deinit(g_hl);
        g_hl = NULL;
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }

    if (xTaskCreate(host_link_poll_task_fn, HOST_LINK_APP_TASK_POLL_NAME, HOST_LINK_APP_TASK_POLL_STACK, NULL,
                    HOST_LINK_APP_TASK_POLL_PRIORITY, &s_poll_task) != pdPASS) {
        vTaskDelete(s_uart_task);
        s_uart_task = NULL;
        host_link_deinit(g_hl);
        g_hl = NULL;
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
        vEventGroupDelete(s_events);
        s_events = NULL;
        return SYS_ERR_NO_MEM;
    }

    (void)bsp_ctrl_register_alarm_in(BSP_ALARM_IN0, hl_alarm_in_host_notify);
    (void)bsp_ctrl_register_alarm_in(BSP_ALARM_IN1, hl_alarm_in_host_notify);

    memset(s_rx_buf, 0, sizeof(s_rx_buf));
    WIC_LOGD("[host_link_app] init OK!");
    return SYS_OK;
}

void host_link_app_deinit(void)
{
    (void)bsp_ctrl_register_alarm_in(BSP_ALARM_IN0, NULL);
    (void)bsp_ctrl_register_alarm_in(BSP_ALARM_IN1, NULL);

    if (s_poll_task != NULL) {
        vTaskDelete(s_poll_task);
        s_poll_task = NULL;
    }
    if (s_uart_task != NULL) {
        vTaskDelete(s_uart_task);
        s_uart_task = NULL;
    }

    (void)HAL_UART_Abort(&huart2);

    if (g_hl != NULL) {
        host_link_deinit(g_hl);
        g_hl = NULL;
    }

    if (s_tx_mutex != NULL) {
        vSemaphoreDelete(s_tx_mutex);
        s_tx_mutex = NULL;
    }

    if (s_events != NULL) {
        vEventGroupDelete(s_events);
        s_events = NULL;
    }
}

host_link_handler_t *host_link_app_handler(void)
{
    return g_hl;
}
