#include "sys_config.h"
#include "rtc.h"
#include "bsp_ctrl.h"
#include "nr_micro_shell.h"
#include "ms41908m.h"
#include "host_link_app.h"
#include "host_link.h"
#include "rs485_driver.h"
#include "ota_module.h"
#include <string.h>

int cmd_rtc(uint8_t argc, char **argv)
{
    RTC_TimeTypeDef sTime;
    RTC_DateTypeDef sDate;

    if (argc == 1U) {
        if (HAL_RTC_GetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: get time failed\r\n");
            return -1;
        }
        if (HAL_RTC_GetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: get date failed\r\n");
            return -1;
        }

        shell_printf("RTC: 20%02d-%02d-%02d %02d:%02d:%02d (WeekDay:%d)\r\n",
                     sDate.Year,
                     sDate.Month,
                     sDate.Date,
                     sTime.Hours,
                     sTime.Minutes,
                     sTime.Seconds,
                     sDate.WeekDay);
        return 0;
    }

    /* rtc set YY MM DD HH MM SS W */
    if (argc == 9U && strcmp(argv[1], "set") == 0) {
        uint8_t year = (uint8_t)atoi(argv[2]);    /* 0-99  */
        uint8_t month = (uint8_t)atoi(argv[3]);   /* 1-12  */
        uint8_t date = (uint8_t)atoi(argv[4]);    /* 1-31  */
        uint8_t hours = (uint8_t)atoi(argv[5]);   /* 0-23  */
        uint8_t minutes = (uint8_t)atoi(argv[6]); /* 0-59  */
        uint8_t seconds = (uint8_t)atoi(argv[7]); /* 0-59  */
        uint8_t weekday = (uint8_t)atoi(argv[8]); /* 1-7   */

        if (year > 99U ||
            month < 1U || month > 12U ||
            date < 1U || date > 31U ||
            hours > 23U ||
            minutes > 59U ||
            seconds > 59U ||
            weekday < 1U || weekday > 7U) {
            shell_printf("rtc: invalid, use YY MM DD HH MM SS W (YY:0-99, M:1-12, D:1-31, H:0-23, M:0-59, S:0-59, W:1-7)\r\n");
            return -1;
        }

        sTime.Hours = hours;
        sTime.Minutes = minutes;
        sTime.Seconds = seconds;
        sTime.TimeFormat = RTC_HOURFORMAT12_AM;
        sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        sTime.StoreOperation = RTC_STOREOPERATION_RESET;

        sDate.Year = year;
        sDate.Month = month;
        sDate.Date = date;
        sDate.WeekDay = weekday;

        if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: set time failed\r\n");
            return -1;
        }

        if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: set date failed\r\n");
            return -1;
        }

        shell_printf("rtc: set to 20%02d-%02d-%02d %02d:%02d:%02d (WeekDay:%d)\r\n",
                     year, month, date, hours, minutes, seconds, weekday);
        return 0;
    }

    if (argc == 5U && (strcmp(argv[1], "set") == 0 || strcmp(argv[1], "settime") == 0)) {
        uint8_t hours = (uint8_t)atoi(argv[2]);
        uint8_t minutes = (uint8_t)atoi(argv[3]);
        uint8_t seconds = (uint8_t)atoi(argv[4]);

        if (hours > 23U || minutes > 59U || seconds > 59U) {
            shell_printf("rtc: invalid time, use HH MM SS (24h)\r\n");
            return -1;
        }

        sTime.Hours = hours;
        sTime.Minutes = minutes;
        sTime.Seconds = seconds;
        sTime.TimeFormat = RTC_HOURFORMAT12_AM;
        sTime.DayLightSaving = RTC_DAYLIGHTSAVING_NONE;
        sTime.StoreOperation = RTC_STOREOPERATION_RESET;

        if (HAL_RTC_SetTime(&hrtc, &sTime, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: set time failed\r\n");
            return -1;
        }

        shell_printf("rtc: time set to %02d:%02d:%02d\r\n", hours, minutes, seconds);
        return 0;
    }

    if (argc == 6U && strcmp(argv[1], "setdate") == 0) {
        uint8_t year = (uint8_t)atoi(argv[2]);   /* 0-99 */
        uint8_t month = (uint8_t)atoi(argv[3]);  /* 1-12 */
        uint8_t date = (uint8_t)atoi(argv[4]);   /* 1-31 */
        uint8_t weekday = (uint8_t)atoi(argv[5]);/* 1-7  */

        if (year > 99U || month < 1U || month > 12U ||
            date < 1U || date > 31U || weekday < 1U || weekday > 7U) {
            shell_printf("rtc: invalid date, use YY MM DD W (YY:0-99, M:1-12, D:1-31, W:1-7)\r\n");
            return -1;
        }

        sDate.Year = year;
        sDate.Month = month;
        sDate.Date = date;
        sDate.WeekDay = weekday;

        if (HAL_RTC_SetDate(&hrtc, &sDate, RTC_FORMAT_BIN) != HAL_OK) {
            shell_printf("rtc: set date failed\r\n");
            return -1;
        }

        shell_printf("rtc: date set to 20%02d-%02d-%02d (WeekDay:%d)\r\n",
                     year, month, date, weekday);
        return 0;
    }

    shell_printf("Usage:\r\n");
    shell_printf("  rtc                                 : show current RTC date/time\r\n");
    shell_printf("  rtc set YY MM DD HH MM SS W         : set full RTC date/time/weekday\r\n");
    shell_printf("  rtc settime HH MM SS                : set RTC time only (24h)\r\n");
    shell_printf("  rtc setdate YY MM DD W              : set RTC date & weekday only\r\n");
    return -1;
}

int cmd_led(uint8_t argc, char **argv)
{
    bsp_led_t led = BSP_LED_MAX;
    uint8_t duty = 0U;
    int ret = SYS_OK;

    if (argc < 2U) {
        shell_printf("Usage:\r\n");
        shell_printf("  led <near|far|w2|w1|ir1> [duty 0-100]\r\n");
        return -1;
    }

    if (strcmp(argv[1], "near") == 0) {
        led = BSP_LED_IR_NEAR;
    } else if (strcmp(argv[1], "far") == 0) {
        led = BSP_LED_IR_FAR;
    } else if (strcmp(argv[1], "w2") == 0) {
        led = BSP_LED_WHITE2;
    } else if (strcmp(argv[1], "w1") == 0) {
        led = BSP_LED_WHITE1;
    } else if (strcmp(argv[1], "ir1") == 0) {
        led = BSP_LED_IR1;
    } else {
        shell_printf("led: invalid led name, use near|far|w2|w1|ir1\r\n");
        return -1;
    }

    if (argc < 3U) {
        duty = bsp_ctrl_get_led_duty(led);
        shell_printf("led: %s duty=%u\r\n", argv[1], duty);
    } else {
        duty = (uint8_t)atoi(argv[2]);
        ret = bsp_ctrl_set_led_duty(led, duty);
        shell_printf("led: %s set duty=%u, ret=%d\r\n", argv[1], duty, ret);
    }
    return ret;
}

int cmd_ircut(uint8_t argc, char **argv)
{
    uint8_t en = 0U;
    int ret = 0;

    if (argc < 2U) {
        en = bsp_ctrl_get_ir_cut();
        shell_printf("ircut: state=%u\r\n", en);
        shell_printf("Usage:\r\n");
        shell_printf("  ircut [0|1]               : get/set IR-CUT state\r\n");
        return 0;
    }

    en = (uint8_t)atoi(argv[1]);
    ret = bsp_ctrl_set_ir_cut(en);
    shell_printf("ircut: set %u, ret=%d\r\n", en, ret);
    return ret;
}

int cmd_fan(uint8_t argc, char **argv)
{
    uint8_t duty = 0U;
    int ret = 0;

    if (argc < 2U) {
        duty = bsp_ctrl_get_fan_duty();
        shell_printf("fan: duty=%u\r\n", duty);
        shell_printf("Usage:\r\n");
        shell_printf("  fan [duty 0-100]          : get/set fan duty (IO on/off)\r\n");
        return 0;
    }

    duty = (uint8_t)atoi(argv[1]);
    ret = bsp_ctrl_set_fan_duty(duty);
    shell_printf("fan: set duty=%u, ret=%d\r\n", duty, ret);
    return ret;
}

int cmd_heat(uint8_t argc, char **argv)
{
    uint8_t duty = 0U;
    int ret = 0;

    if (argc < 2U) {
        duty = bsp_ctrl_get_heat_duty();
        shell_printf("heat: duty=%u\r\n", duty);
        shell_printf("Usage:\r\n");
        shell_printf("  heat [duty 0-100]         : get/set heater duty (IO on/off)\r\n");
        return 0;
    }

    duty = (uint8_t)atoi(argv[1]);
    ret = bsp_ctrl_set_heat_duty(duty);
    shell_printf("heat: set duty=%u, ret=%d\r\n", duty, ret);
    return ret;
}

int cmd_radar(uint8_t argc, char **argv)
{
    uint8_t en = 0U;
    int ret = 0;

    if (argc < 2U) {
        en = bsp_ctrl_get_radar_en();
        shell_printf("radar: state=%u\r\n", en);
        shell_printf("Usage:\r\n");
        shell_printf("  radar [0|1]               : get/set radar enable\r\n");
        return 0;
    }

    en = (uint8_t)atoi(argv[1]);
    ret = bsp_ctrl_set_radar_en(en);
    shell_printf("radar: set %u, ret=%d\r\n", en, ret);
    return ret;
}

int cmd_aout(uint8_t argc, char **argv)
{
    int ret = 0;

    if (argc < 2U) {
        shell_printf("Usage:\r\n");
        shell_printf("  aout <0|1> [0|1]          : get/set alarm out0/1\r\n");
        return -1;
    }

    bsp_alarm_out_t out = (bsp_alarm_out_t)atoi(argv[1]);
    if (argc < 3U) {
        uint8_t state = bsp_ctrl_get_alarm_out(out);
        shell_printf("aout%u: state=%u\r\n", (unsigned)out, state);
        return 0;
    }

    uint8_t en = (uint8_t)atoi(argv[2]);
    ret = bsp_ctrl_set_alarm_out(out, en);
    shell_printf("aout%u: set %u, ret=%d\r\n", (unsigned)out, en, ret);
    return ret;
}

int cmd_wout(uint8_t argc, char **argv)
{
    int ret = 0;

    if (argc < 2U) {
        shell_printf("Usage:\r\n");
        shell_printf("  wout <0|1> [0|1]           : get/set Wiegand out0/1\r\n");
        return -1;
    }

    bsp_wiegand_t w = (bsp_wiegand_t)atoi(argv[1]);
    if (argc < 3U) {
        uint8_t state = bsp_ctrl_get_wiegand(w);
        shell_printf("wout%u: state=%u\r\n", (unsigned)w, state);
        return 0;
    }

    uint8_t en = (uint8_t)atoi(argv[2]);
    ret = bsp_ctrl_set_wiegand(w, en);
    shell_printf("wout%u: set %u, ret=%d\r\n", (unsigned)w, en, ret);
    return ret;
}

int cmd_pd(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint16_t mv = bsp_ctrl_get_pd_voltage_mv();
    float val = bsp_ctrl_convert_pd_to_lux(mv);
    shell_printf("pd: voltage=%u mV, value=%.3f lux\r\n", mv, val);
    return 0;
}

int cmd_temp(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;

    uint16_t mv = bsp_ctrl_get_temp_voltage_mv();
    float val = bsp_ctrl_convert_lmt87_to_celcius(mv);
    shell_printf("temp: voltage=%u mV, value=%.3f C\r\n", mv, val);
    return 0;
}

void cmd_ain_callback(bsp_alarm_in_t in, uint8_t level)
{
    shell_printf("ain%u: level=%d\r\n", (unsigned)in, level);
}

int cmd_ain(uint8_t argc, char **argv)
{
    int ret = 0;

    if (argc < 2U) {
        shell_printf("Usage:\r\n");
        shell_printf("  ain <0|1>                 : read alarm in0/1 level\r\n");
        shell_printf("  ain <0|1> register        : register callback for alarm in0/1\r\n");
        return -1;
    }

    if (argc > 2U) {
        if (strcmp(argv[2], "register") == 0) {
            bsp_alarm_in_t in = (bsp_alarm_in_t)atoi(argv[1]);
            ret = bsp_ctrl_register_alarm_in(in, cmd_ain_callback);
            shell_printf("ain%u: register callback, ret=%d\r\n", (unsigned)in, ret);
            return ret;
        }
        shell_printf("Usage:\r\n");
        shell_printf("  ain <0|1>                 : read alarm in0/1 level\r\n");
        shell_printf("  ain <0|1> register        : register callback for alarm in0/1\r\n");
        return -1;
    }

    bsp_alarm_in_t in = (bsp_alarm_in_t)atoi(argv[1]);
    ret = bsp_ctrl_get_alarm_in(in);
    shell_printf("ain%u: level=%d\r\n", (unsigned)in, ret);
    return ret < 0 ? -1 : 0;
}

int cmd_reset_soc(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;

    int ret = bsp_ctrl_reset_soc_power();
    shell_printf("reset_soc: ret=%d\r\n", ret);
    return ret;
}

static const char *ota_app_status_str(ota_app_status_t s)
{
    switch (s) {
    case OTA_APP_STATUS_INVALID: return "INVALID";
    case OTA_APP_STATUS_DOWNLOADING: return "DOWNLOADING";
    case OTA_APP_STATUS_UNVERIFIED: return "UNVERIFIED";
    case OTA_APP_STATUS_ACTIVE: return "ACTIVE";
    case OTA_APP_STATUS_BACKUP: return "BACKUP";
    default: return "?";
    }
}

int cmd_reboot(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    shell_printf("reboot: now\r\n");
    vTaskDelay(pdMS_TO_TICKS(50));
    NVIC_SystemReset();
    return 0;
}

int cmd_boot_ota(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;
    int rc = ota_module_set_boot_ymodem_flag(1u);
    shell_printf("boot_ota: set flag=1 rc=%d, reboot...\r\n", rc);
    vTaskDelay(pdMS_TO_TICKS(50));
    NVIC_SystemReset();
    return 0;
}

int cmd_ota_info(uint8_t argc, char **argv)
{
    (void)argc;
    (void)argv;

    ota_record_info_t info;
    memset(&info, 0, sizeof(info));
    int rc = ota_module_get_record_info(&info);
    shell_printf("ota_info: rc=%d\r\n", rc);
    shell_printf("  magic=0x%08lX boot_ymodem_flag=%lu info_crc32=0x%08lX\r\n",
                 (unsigned long)info.magic,
                 (unsigned long)info.boot_ymodem_flag,
                 (unsigned long)info.info_crc32);

    shell_printf("  slot0: status=%s verify_count=%lu flash_type=%lu addr=0x%08lX size=%lu\r\n",
                 ota_app_status_str(info.ota_app_info[0].status),
                 (unsigned long)info.ota_app_info[0].verify_count,
                 (unsigned long)info.ota_app_info[0].flash_type,
                 (unsigned long)info.ota_app_info[0].flash_addr,
                 (unsigned long)info.ota_app_info[0].flash_size);
    return 0;
}

/* MS41908M driver test commands */

static const char *cmd_lens_state_name(ms41908m_state_t s)
{
    switch (s) {
    case MS41908M_STATE_NO_CFG:
        return "NO_CFG";
    case MS41908M_STATE_STOPPED:
        return "STOPPED";
    case MS41908M_STATE_RUNNING:
        return "RUNNING";
    case MS41908M_STATE_RESET_ZERO:
        return "RESET_ZERO";
    case MS41908M_STATE_ERROR:
        return "ERROR";
    default:
        return "?";
    }
}

static void cmd_lens_usage(void)
{
    shell_printf("MS41908M lens test (call init -> cfg -> iris/zoom/focus):\r\n");
    shell_printf("  lens init|deinit\r\n");
    shell_printf("  lens cfg [all|iris|motor]     : load defaults (default: all)\r\n");
    shell_printf("  lens state                    : iris/zoom/focus state + positions\r\n");
    shell_printf("  lens iris run|stop|adc|tgt <0-1023>\r\n");
    shell_printf("  lens zoom|focus run <pps> <micro_steps>   (+ => max, - => min)\r\n");
    shell_printf("  lens zoom|focus abs <pps> <position>      (absolute; need zero, limits if configured)\r\n");
    shell_printf("  lens zoom|focus stop\r\n");
    shell_printf("  lens zoom|focus wait [timeout_ms]        (default 5000)\r\n");
    shell_printf("  lens zoom|focus rz [timeout_ms]          : async reset zero (default wait 120s)\r\n");
    shell_printf("  lens zoom|focus lim <min> <max>          : position limits for reset\r\n");
    shell_printf("  lens pi [zoom|focus]                   : read PI level (1=HIGH, 0=LOW)\r\n");
}

static void cmd_lens_pi_print(const char *axis, int pi, int is_zoom)
{
    const char *level = (pi != 0) ? "HIGH" : "LOW";

    shell_printf("lens: pi_%s=%d (%s)", axis, pi, level);
    if (pi < 0) {
        shell_printf(" (not initialized)\r\n");
        return;
    }
    if (is_zoom) {
        if (pi != 0) {
            shell_printf(", clear at max(+)\r\n");
        } else {
            shell_printf(", shaded at min(-)\r\n");
        }
    } else {
        if (pi != 0) {
            shell_printf(", clear at min(-)\r\n");
        } else {
            shell_printf(", shaded at max(+)\r\n");
        }
    }
}

int cmd_lens(uint8_t argc, char **argv)
{
    int ret;
    uint32_t timeout_ms;

    if (argc < 2U) {
        cmd_lens_usage();
        return -1;
    }

    if (strcmp(argv[1], "init") == 0) {
        ret = ms41908m_init();
        shell_printf("lens: init ret=%d\r\n", ret);
        return (ret == SYS_OK) ? 0 : -1;
    }

    if (strcmp(argv[1], "deinit") == 0) {
        ms41908m_deinit();
        shell_printf("lens: deinit done\r\n");
        return 0;
    }

    if (strcmp(argv[1], "cfg") == 0) {
        const char *what = "all";

        if (argc >= 3U) {
            what = argv[2];
        }
        if (strcmp(what, "all") == 0) {
            ret = ms41908m_iris_config(&g_default_iris_config);
            if (ret != SYS_OK) {
                shell_printf("lens: iris cfg ret=%d\r\n", ret);
                return -1;
            }
            ret = ms41908m_motor_config(&g_default_motor_config);
            shell_printf("lens: cfg all ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(what, "iris") == 0) {
            ret = ms41908m_iris_config(&g_default_iris_config);
            shell_printf("lens: cfg iris ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(what, "motor") == 0) {
            ret = ms41908m_motor_config(&g_default_motor_config);
            shell_printf("lens: cfg motor ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        shell_printf("lens: cfg unknown '%s', use all|iris|motor\r\n", what);
        return -1;
    }

    if (strcmp(argv[1], "state") == 0) {
        int pi_z = ms41908m_read_pi_zoom();
        int pi_f = ms41908m_read_pi_focus();

        shell_printf("lens: iris=%s zoom=%s focus=%s pos_z=%d pos_f=%d\r\n",
                     cmd_lens_state_name(ms41908m_iris_get_state()),
                     cmd_lens_state_name(ms41908m_get_zoom_state()),
                     cmd_lens_state_name(ms41908m_get_focus_state()),
                     ms41908m_get_zoom_position(),
                     ms41908m_get_focus_position());
        shell_printf("lens: zoom_rz_done=%d focus_rz_done=%d\r\n",
                     ms41908m_zoom_is_reset_zero(),
                     ms41908m_focus_is_reset_zero());
        if (pi_z >= 0) {
            cmd_lens_pi_print("zoom", pi_z, 1);
        }
        if (pi_f >= 0) {
            cmd_lens_pi_print("focus", pi_f, 0);
        }
        return 0;
    }

    if (strcmp(argv[1], "pi") == 0) {
        if (argc >= 3U && strcmp(argv[2], "zoom") == 0) {
            cmd_lens_pi_print("zoom", ms41908m_read_pi_zoom(), 1);
            return 0;
        }
        if (argc >= 3U && strcmp(argv[2], "focus") == 0) {
            cmd_lens_pi_print("focus", ms41908m_read_pi_focus(), 0);
            return 0;
        }
        cmd_lens_pi_print("zoom", ms41908m_read_pi_zoom(), 1);
        cmd_lens_pi_print("focus", ms41908m_read_pi_focus(), 0);
        return 0;
    }

    if (strcmp(argv[1], "iris") == 0) {
        if (argc < 3U) {
            shell_printf("lens: iris run|stop|adc|tgt <0-1023>\r\n");
            return -1;
        }
        if (strcmp(argv[2], "run") == 0) {
            ret = ms41908m_iris_run();
            shell_printf("lens: iris run ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "stop") == 0) {
            ret = ms41908m_iris_stop();
            shell_printf("lens: iris stop ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "adc") == 0) {
            uint16_t adc = ms41908m_iris_read_adc();
            shell_printf("lens: iris adc=%u\r\n", (unsigned)adc);
            return 0;
        }
        if (strcmp(argv[2], "tgt") == 0) {
            if (argc < 4U) {
                shell_printf("lens: iris tgt <0-1023>\r\n");
                return -1;
            }
            uint16_t tgt = (uint16_t)strtoul(argv[3], NULL, 0);
            ret = ms41908m_iris_update_target(tgt);
            shell_printf("lens: iris tgt=%u ret=%d\r\n", (unsigned)tgt, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        shell_printf("lens: iris unknown subcmd\r\n");
        return -1;
    }

    if (strcmp(argv[1], "zoom") == 0) {
        if (argc < 3U) {
            shell_printf("lens: zoom run|abs|stop|wait|rz|lim ...\r\n");
            return -1;
        }
        if (strcmp(argv[2], "run") == 0) {
            if (argc < 5U) {
                shell_printf("lens: zoom run <pps> <micro_steps>\r\n");
                return -1;
            }
            uint16_t pps = (uint16_t)strtoul(argv[3], NULL, 0);
            long steps = strtol(argv[4], NULL, 0);
            ret = ms41908m_zoom_run(pps, (int32_t)steps);
            shell_printf("lens: zoom run pps=%u steps=%ld ret=%d\r\n",
                         (unsigned)pps, steps, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "abs") == 0) {
            if (argc < 5U) {
                shell_printf("lens: zoom abs <pps> <position>\r\n");
                return -1;
            }
            uint16_t pps = (uint16_t)strtoul(argv[3], NULL, 0);
            long pos = strtol(argv[4], NULL, 0);
            ret = ms41908m_zoom_run_to_position(pps, (int32_t)pos);
            shell_printf("lens: zoom abs pps=%u pos=%ld ret=%d\r\n",
                         (unsigned)pps, pos, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "stop") == 0) {
            ret = ms41908m_zoom_stop();
            shell_printf("lens: zoom stop ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "wait") == 0) {
            timeout_ms = (argc >= 4U) ? (uint32_t)strtoul(argv[3], NULL, 0) : 5000U;
            ret = ms41908m_zoom_wait_for_completion(timeout_ms);
            shell_printf("lens: zoom wait %lu ms ret=%d\r\n", (unsigned long)timeout_ms, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "rz") == 0) {
            timeout_ms = (argc >= 4U) ? (uint32_t)strtoul(argv[3], NULL, 0) : 120000U;
            ret = ms41908m_zoom_reset_zero();
            shell_printf("lens: zoom rz queue ret=%d\r\n", ret);
            if (ret == SYS_OK) {
                ret = ms41908m_zoom_wait_reset_done(timeout_ms);
                shell_printf("lens: zoom rz wait ret=%d pos=%d\r\n", ret, ms41908m_get_zoom_position());
            }
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "lim") == 0) {
            if (argc < 5U) {
                shell_printf("lens: zoom lim <min> <max>\r\n");
                return -1;
            }
            int32_t mn = (int32_t)strtol(argv[3], NULL, 0);
            int32_t mx = (int32_t)strtol(argv[4], NULL, 0);
            ret = ms41908m_zoom_set_position_limit(mn, mx);
            shell_printf("lens: zoom lim %ld %ld ret=%d\r\n", (long)mn, (long)mx, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        shell_printf("lens: zoom unknown subcmd\r\n");
        return -1;
    }

    if (strcmp(argv[1], "focus") == 0) {
        if (argc < 3U) {
            shell_printf("lens: focus run|abs|stop|wait|rz|lim ...\r\n");
            return -1;
        }
        if (strcmp(argv[2], "run") == 0) {
            if (argc < 5U) {
                shell_printf("lens: focus run <pps> <micro_steps>\r\n");
                return -1;
            }
            uint16_t pps = (uint16_t)strtoul(argv[3], NULL, 0);
            long steps = strtol(argv[4], NULL, 0);
            ret = ms41908m_focus_run(pps, (int32_t)steps);
            shell_printf("lens: focus run pps=%u steps=%ld ret=%d\r\n",
                         (unsigned)pps, steps, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "abs") == 0) {
            if (argc < 5U) {
                shell_printf("lens: focus abs <pps> <position>\r\n");
                return -1;
            }
            uint16_t pps = (uint16_t)strtoul(argv[3], NULL, 0);
            long pos = strtol(argv[4], NULL, 0);
            ret = ms41908m_focus_run_to_position(pps, (int32_t)pos);
            shell_printf("lens: focus abs pps=%u pos=%ld ret=%d\r\n",
                         (unsigned)pps, pos, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "stop") == 0) {
            ret = ms41908m_focus_stop();
            shell_printf("lens: focus stop ret=%d\r\n", ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "wait") == 0) {
            timeout_ms = (argc >= 4U) ? (uint32_t)strtoul(argv[3], NULL, 0) : 5000U;
            ret = ms41908m_focus_wait_for_completion(timeout_ms);
            shell_printf("lens: focus wait %lu ms ret=%d\r\n", (unsigned long)timeout_ms, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "rz") == 0) {
            timeout_ms = (argc >= 4U) ? (uint32_t)strtoul(argv[3], NULL, 0) : 120000U;
            ret = ms41908m_focus_reset_zero();
            shell_printf("lens: focus rz queue ret=%d\r\n", ret);
            if (ret == SYS_OK) {
                ret = ms41908m_focus_wait_reset_done(timeout_ms);
                shell_printf("lens: focus rz wait ret=%d pos=%d\r\n", ret, ms41908m_get_focus_position());
            }
            return (ret == SYS_OK) ? 0 : -1;
        }
        if (strcmp(argv[2], "lim") == 0) {
            if (argc < 5U) {
                shell_printf("lens: focus lim <min> <max>\r\n");
                return -1;
            }
            int32_t mn = (int32_t)strtol(argv[3], NULL, 0);
            int32_t mx = (int32_t)strtol(argv[4], NULL, 0);
            ret = ms41908m_focus_set_position_limit(mn, mx);
            shell_printf("lens: focus lim %ld %ld ret=%d\r\n", (long)mn, (long)mx, ret);
            return (ret == SYS_OK) ? 0 : -1;
        }
        shell_printf("lens: focus unknown subcmd\r\n");
        return -1;
    }

    cmd_lens_usage();
    return -1;
}

static void cmd_hl_usage(void)
{
    shell_printf("hl — host_link test (USART2 -> Linux, peer must handle PING/ECHO):\r\n");
    shell_printf("  hl              : help\r\n");
    shell_printf("  hl st           : handler init status\r\n");
    shell_printf("  hl ping [-n <N>] [-i <ms>] [-t <timeout_ms>] : single by default; print min/avg/max RTT\r\n");
    shell_printf("  hl echo <text>  : REQUEST ECHO (spaces OK)\r\n");
    shell_printf("  hl evt [text]   : EVENT cmd=ECHO, wait EVENT_ACK\r\n");
}

static void cmd_hl_print_resp(const void *data, uint16_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    unsigned i;
    int printable = 1;

    if (data == NULL || len == 0U) {
        shell_printf("(empty)\r\n");
        return;
    }
    for (i = 0; i < len; i++) {
        if (p[i] < 0x20u || p[i] > 0x7eu) {
            printable = 0;
            break;
        }
    }
    if (printable) {
        shell_printf("\"");
        for (i = 0; i < len; i++) {
            shell_printf("%c", (char)p[i]);
        }
        shell_printf("\"\r\n");
    } else {
        shell_printf("hex %u: ", (unsigned)len);
        for (i = 0; i < len && i < 64U; i++) {
            shell_printf("%02X ", (unsigned)p[i]);
        }
        if (len > 64U) {
            shell_printf("...");
        }
        shell_printf("\r\n");
    }
}

int cmd_host_link(uint8_t argc, char **argv)
{
    host_link_handler_t *h;
    void *resp = NULL;
    uint16_t resp_len = 0;
    int ret;
    char buf[256];
    uint16_t plen;
    uint8_t i;

    if (argc < 2) {
        cmd_hl_usage();
        return 0;
    }

    h = host_link_app_handler();
    if (strcmp(argv[1], "st") == 0) {
        shell_printf("hl: handler=%s\r\n", (h != NULL) ? "ok" : "NULL (init failed?)");
        return 0;
    }

    if (h == NULL) {
        shell_printf("hl: not initialized\r\n");
        return -1;
    }

    if (strcmp(argv[1], "ping") == 0) {
        uint32_t count = 1U;
        uint32_t interval_ms = 0U;
        uint32_t timeout_ms = 0U;
        uint32_t i_ping;
        uint32_t ok = 0U;
        uint32_t min_ms = 0xFFFFFFFFu;
        uint32_t max_ms = 0U;
        uint64_t sum_ms = 0ULL;
        uint8_t ai;

        for (ai = 2U; ai + 1U < argc; ai = (uint8_t)(ai + 2U)) {
            if (strcmp(argv[ai], "-n") == 0) {
                count = (uint32_t)strtoul(argv[ai + 1U], NULL, 0);
                if (count == 0U) {
                    count = 1U;
                }
            } else if (strcmp(argv[ai], "-i") == 0) {
                interval_ms = (uint32_t)strtoul(argv[ai + 1U], NULL, 0);
            } else if (strcmp(argv[ai], "-t") == 0) {
                timeout_ms = (uint32_t)strtoul(argv[ai + 1U], NULL, 0);
            } else {
                shell_printf("hl: ping args error, use: hl ping [-n <N>] [-i <ms>] [-t <timeout_ms>]\r\n");
                return -1;
            }
        }

        for (i_ping = 0U; i_ping < count; i_ping++) {
            uint32_t t0 = HAL_GetTick();
            uint32_t dt;
            ret = host_link_request(h, HOST_LINK_CMD_PING, NULL, 0, &resp, &resp_len);
            dt = HAL_GetTick() - t0;

            if (ret == HOST_LINK_OK && timeout_ms > 0U && dt > timeout_ms) {
                ret = HOST_LINK_ERR_TIMEOUT;
            }

            if (ret != HOST_LINK_OK) {
                shell_printf("hl: ping [%lu/%lu] err=%d rtt=%lu ms\r\n",
                             (unsigned long)(i_ping + 1U), (unsigned long)count, ret, (unsigned long)dt);
                continue;
            }

            ok++;
            if (dt < min_ms) {
                min_ms = dt;
            }
            if (dt > max_ms) {
                max_ms = dt;
            }
            sum_ms += (uint64_t)dt;

            if (resp_len >= 4U && resp != NULL) {
                uint32_t v;
                memcpy(&v, resp, sizeof(v));
                shell_printf("hl: ping [%lu/%lu] OK, u32=0x%08lX (%lu), rtt=%lu ms\r\n",
                             (unsigned long)(i_ping + 1U), (unsigned long)count,
                             (unsigned long)v, (unsigned long)v, (unsigned long)dt);
            } else {
                shell_printf("hl: ping [%lu/%lu] OK, len=%u, rtt=%lu ms\r\n",
                             (unsigned long)(i_ping + 1U), (unsigned long)count,
                             (unsigned)resp_len, (unsigned long)dt);
                cmd_hl_print_resp(resp, resp_len);
            }

            if (resp != NULL) {
                host_link_free(resp);
                resp = NULL;
            }
            resp_len = 0U;
            if (interval_ms > 0U && (i_ping + 1U) < count) {
                HAL_Delay(interval_ms);
            }
        }

        if (count > 1U) {
            if (ok > 0U) {
                shell_printf("hl: ping stats sent=%lu ok=%lu loss=%lu min/avg/max=%lu/%lu/%lu ms\r\n",
                             (unsigned long)count, (unsigned long)ok, (unsigned long)(count - ok),
                             (unsigned long)min_ms, (unsigned long)(sum_ms / ok), (unsigned long)max_ms);
            } else {
                shell_printf("hl: ping stats sent=%lu ok=0 loss=%lu\r\n",
                             (unsigned long)count, (unsigned long)count);
            }
        }

        if (resp != NULL) {
            host_link_free(resp);
            resp = NULL;
        }
        return (ok > 0U) ? 0 : -1;
    }

    if (strcmp(argv[1], "echo") == 0) {
        if (argc < 3) {
            shell_printf("hl: echo <text>\r\n");
            return -1;
        }
        plen = 0;
        for (i = 2; i < argc && plen + 1U < sizeof(buf); i++) {
            size_t L;
            if (i > 2U && plen < sizeof(buf) - 1U) {
                buf[plen++] = (char)' ';
            }
            L = strlen(argv[i]);
            if (plen + L >= sizeof(buf)) {
                L = sizeof(buf) - 1U - (size_t)plen;
            }
            if (L > 0U) {
                memcpy(buf + plen, argv[i], L);
                plen += (uint16_t)L;
            }
        }
        ret = host_link_request(h, HOST_LINK_CMD_ECHO, buf, plen, &resp, &resp_len);
        if (ret != HOST_LINK_OK) {
            shell_printf("hl: echo err=%d\r\n", ret);
            return -1;
        }
        shell_printf("hl: echo OK, len=%u: ", (unsigned)resp_len);
        cmd_hl_print_resp(resp, resp_len);
        if (resp != NULL) {
            host_link_free(resp);
        }
        return 0;
    }

    if (strcmp(argv[1], "evt") == 0) {
        plen = 0;
        if (argc >= 3) {
            for (i = 2; i < argc && plen + 1U < sizeof(buf); i++) {
                size_t L;
                if (i > 2U && plen < sizeof(buf) - 1U) {
                    buf[plen++] = (char)' ';
                }
                L = strlen(argv[i]);
                if (plen + L >= sizeof(buf)) {
                    L = sizeof(buf) - 1U - (size_t)plen;
                }
                if (L > 0U) {
                    memcpy(buf + plen, argv[i], L);
                    plen += (uint16_t)L;
                }
            }
        }
        ret = host_link_send_event(h, HOST_LINK_CMD_ECHO, (plen > 0U) ? buf : NULL, plen);
        if (ret != HOST_LINK_OK) {
            shell_printf("hl: evt err=%d\r\n", ret);
            return -1;
        }
        shell_printf("hl: evt OK (EVENT + EVENT_ACK)\r\n");
        return 0;
    }

    cmd_hl_usage();
    return -1;
}

int cmd_rs485(uint8_t argc, char **argv)
{
    int ret;
    char tx_buf[256];
    uint8_t rx_buf[RS485_RX_BUFFER_SIZE];
    uint16_t out_len = 0u;
    uint16_t tx_len = 0u;
    uint32_t timeout_ms = RS485_DEFAULT_TIMEOUT_MS;
    uint8_t i;

    if (argc < 2U) {
        shell_printf("RS485 test (USART3):\r\n");
        shell_printf("  rs485 st\r\n");
        shell_printf("  rs485 init [baud] [cfg(8N1)]\r\n");
        shell_printf("  rs485 deinit\r\n");
        shell_printf("  rs485 tx <text>\r\n");
        shell_printf("  rs485 rx [timeout_ms] [max_len]\r\n");
        shell_printf("  rs485 txrx <timeout_ms> <text>\r\n");
        return 0;
    }

    if (strcmp(argv[1], "st") == 0) {
        shell_printf("rs485: %s\r\n", rs485_driver_is_inited() ? "inited" : "not inited");
        return 0;
    }

    if (strcmp(argv[1], "init") == 0) {
        uint32_t baud = 115200u;
        const char *cfg = "8N1";
        if (argc >= 3U) {
            baud = (uint32_t)strtoul(argv[2], NULL, 0);
        }
        if (argc >= 4U) {
            cfg = argv[3];
        }
        ret = rs485_driver_init(baud, cfg);
        shell_printf("rs485: init baud=%lu cfg=%s ret=%d\r\n", (unsigned long)baud, cfg, ret);
        return (ret == SYS_OK) ? 0 : -1;
    }

    if (strcmp(argv[1], "deinit") == 0) {
        ret = rs485_driver_deinit();
        shell_printf("rs485: deinit ret=%d\r\n", ret);
        return (ret == SYS_OK) ? 0 : -1;
    }

    if (strcmp(argv[1], "tx") == 0) {
        if (argc < 3U) {
            shell_printf("rs485: tx <text>\r\n");
            return -1;
        }
        tx_len = 0u;
        for (i = 2u; i < argc && tx_len + 1u < (uint16_t)sizeof(tx_buf); i++) {
            size_t L;
            if (i > 2u) {
                tx_buf[tx_len++] = ' ';
            }
            L = strlen(argv[i]);
            if (tx_len + (uint16_t)L >= (uint16_t)sizeof(tx_buf)) {
                L = (size_t)((uint16_t)sizeof(tx_buf) - tx_len);
            }
            if (L > 0u) {
                memcpy(&tx_buf[tx_len], argv[i], L);
                tx_len += (uint16_t)L;
            }
        }
        ret = rs485_driver_send((const uint8_t *)tx_buf, tx_len, timeout_ms);
        shell_printf("rs485: tx len=%u ret=%d\r\n", (unsigned)tx_len, ret);
        return (ret == SYS_OK) ? 0 : -1;
    }

    if (strcmp(argv[1], "rx") == 0) {
        uint16_t max_len = 128u;
        if (argc >= 3U) {
            timeout_ms = (uint32_t)strtoul(argv[2], NULL, 0);
        }
        if (argc >= 4U) {
            max_len = (uint16_t)strtoul(argv[3], NULL, 0);
        }
        if (max_len > RS485_RX_BUFFER_SIZE) {
            max_len = RS485_RX_BUFFER_SIZE;
        }
        ret = rs485_driver_recv(rx_buf, max_len, &out_len, timeout_ms);
        if (ret != SYS_OK) {
            shell_printf("rs485: rx ret=%d\r\n", ret);
            return -1;
        }
        shell_printf("rs485: rx len=%u hex: ", (unsigned)out_len);
        for (i = 0u; i < out_len; i++) {
            shell_printf("%02X ", (unsigned)rx_buf[i]);
        }
        shell_printf("\r\n");
        return 0;
    }

    if (strcmp(argv[1], "txrx") == 0) {
        if (argc < 4U) {
            shell_printf("rs485: txrx <timeout_ms> <text>\r\n");
            return -1;
        }
        timeout_ms = (uint32_t)strtoul(argv[2], NULL, 0);
        tx_len = 0u;
        for (i = 3u; i < argc && tx_len + 1u < (uint16_t)sizeof(tx_buf); i++) {
            size_t L;
            if (i > 3u) {
                tx_buf[tx_len++] = ' ';
            }
            L = strlen(argv[i]);
            if (tx_len + (uint16_t)L >= (uint16_t)sizeof(tx_buf)) {
                L = (size_t)((uint16_t)sizeof(tx_buf) - tx_len);
            }
            if (L > 0u) {
                memcpy(&tx_buf[tx_len], argv[i], L);
                tx_len += (uint16_t)L;
            }
        }
        ret = rs485_driver_txrx((const uint8_t *)tx_buf, tx_len, rx_buf, sizeof(rx_buf), &out_len, timeout_ms);
        if (ret != SYS_OK) {
            shell_printf("rs485: txrx ret=%d\r\n", ret);
            return -1;
        }
        shell_printf("rs485: txrx rx_len=%u hex: ", (unsigned)out_len);
        for (i = 0u; i < out_len; i++) {
            shell_printf("%02X ", (unsigned)rx_buf[i]);
        }
        shell_printf("\r\n");
        return 0;
    }

    shell_printf("rs485: unknown subcmd\r\n");
    return -1;
}

