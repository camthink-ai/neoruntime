/**
 * @file host_link_proto.h
 * @brief Wire format for host MCU <-> Linux bridging (little-endian on the wire).
 *
 * Header CRC covers all bytes before hdr_crc. Payload CRC covers payload only.
 *
 * NOTE: This file is intentionally kept inside hal_v2 to avoid coupling the
 * V2 implementation to the V1 directory layout.
 */
#pragma once

#include <stdint.h>

/* 0x4C48 — 'H'(0x48) 'L'(0x4C) on wire as little-endian uint16_t */
#define HOST_LINK_MAGIC           ((uint16_t)0x4C48u)
#define HOST_LINK_VERSION_CURRENT ((uint8_t)1u)

/* OTA download payload embeds a packed header from MCU-side ota_module.h. */
#define HOST_LINK_OTA_PACKAGE_MAGIC                ((uint32_t)0x5441544Fu) /* 'TAT O' */
#define HOST_LINK_OTA_PACKAGE_VERSION_MAX_LEN    32u

/** Bytes before hdr_crc (magic .. len inclusive). */
#define HOST_LINK_HDR_CRC_LEN     (12u)
/** Full on-wire header size including hdr_crc. */
#define HOST_LINK_HEADER_WIRE_LEN (14u)

typedef enum {
    HOST_LINK_OK = 0,
    HOST_LINK_ERR_INVALID_ARG = -1,
    HOST_LINK_ERR_INVALID_STATE = -2,
    HOST_LINK_ERR_INVALID_SIZE = -3,
    HOST_LINK_ERR_NO_MEM = -4,
    HOST_LINK_ERR_NO_SLOT = -5,
    HOST_LINK_ERR_TIMEOUT = -6,
    HOST_LINK_ERR_CRC = -7,
    HOST_LINK_ERR_SEND = -8,
    HOST_LINK_ERR_PROTO = -9,
} host_link_err_t;

typedef enum {
    HOST_LINK_TYPE_REQUEST = 0,
    HOST_LINK_TYPE_RESPONSE = 1,
    HOST_LINK_TYPE_EVENT = 2,
    HOST_LINK_TYPE_EVENT_ACK = 3,
} host_link_frame_type_t;

/**
 * Command IDs (uint16_t on wire). 0–0x000F reserved / well-known; 0x0010+ map to ne503 MCU.
 */
typedef enum {
    HOST_LINK_CMD_PING = 0x0000,
    HOST_LINK_CMD_ECHO = 0x0001,

    HOST_LINK_CMD_GET_VERSION = 0x0010,
    HOST_LINK_CMD_RTC_GET = 0x0011,
    HOST_LINK_CMD_RTC_SET = 0x0012,

    HOST_LINK_CMD_LED_SET = 0x0020,
    HOST_LINK_CMD_LED_GET = 0x0021,
    HOST_LINK_CMD_IRCUT_SET = 0x0022,
    HOST_LINK_CMD_IRCUT_GET = 0x0023,
    HOST_LINK_CMD_PD_GET = 0x0024,
    HOST_LINK_CMD_TEMP_GET = 0x0025,
    HOST_LINK_CMD_FAN_SET = 0x0026,
    HOST_LINK_CMD_FAN_GET = 0x0027,
    HOST_LINK_CMD_HEAT_SET = 0x0028,
    HOST_LINK_CMD_HEAT_GET = 0x0029,
    HOST_LINK_CMD_RADAR_SET = 0x002A,
    HOST_LINK_CMD_RADAR_GET = 0x002B,
    HOST_LINK_CMD_AOUT_SET = 0x002C,
    HOST_LINK_CMD_AOUT_GET = 0x002D,
    HOST_LINK_CMD_WOUT_SET = 0x002E,
    HOST_LINK_CMD_WOUT_GET = 0x002F,
    HOST_LINK_CMD_AIN_GET = 0x0030,
    HOST_LINK_CMD_RESET_SOC = 0x0031,
    HOST_LINK_CMD_RS485_INIT = 0x0032,
    HOST_LINK_CMD_RS485_DEINIT = 0x0033,
    HOST_LINK_CMD_RS485_TX = 0x0034,

    HOST_LINK_CMD_LENS_INIT = 0x0038,
    HOST_LINK_CMD_LENS_DEINIT = 0x0039,
    HOST_LINK_CMD_LENS_CFG = 0x003A,
    HOST_LINK_CMD_LENS_STATE_GET = 0x003B,
    HOST_LINK_CMD_LENS_IRIS_RUN = 0x003C,
    HOST_LINK_CMD_LENS_IRIS_STOP = 0x003D,
    HOST_LINK_CMD_LENS_IRIS_TGT_SET = 0x003E,
    HOST_LINK_CMD_LENS_IRIS_ADC_GET = 0x003F,
    HOST_LINK_CMD_EV_ALARM_IN = 0x0040,
    HOST_LINK_CMD_EV_RS485_RX = 0x0041,
    HOST_LINK_CMD_LENS_ZOOM_RUN = 0x0042,
    HOST_LINK_CMD_LENS_ZOOM_ABS = 0x0043,
    HOST_LINK_CMD_LENS_ZOOM_STOP = 0x0044,
    HOST_LINK_CMD_LENS_ZOOM_RZ = 0x0045,
    HOST_LINK_CMD_LENS_ZOOM_LIM_SET = 0x0046,
    HOST_LINK_CMD_LENS_FOCUS_RUN = 0x0047,
    HOST_LINK_CMD_LENS_FOCUS_ABS = 0x0048,
    HOST_LINK_CMD_LENS_FOCUS_STOP = 0x0049,
    HOST_LINK_CMD_LENS_FOCUS_RZ = 0x004A,
    HOST_LINK_CMD_LENS_FOCUS_LIM_SET = 0x004B,
    HOST_LINK_CMD_EV_LENS = 0x004C,
    HOST_LINK_CMD_LENS_ZF_SYNC_RUN = 0x004D,

    /* OTA (mcu_board_prj/app/Custom/Components/ota_host): tag + MCU reset; bootloader does Ymodem. */
    HOST_LINK_CMD_OTA_ENTER_BOOT = 0x0050,
    /** MCU NVIC_SystemReset (no Ymodem tag). */
    HOST_LINK_CMD_REBOOT = 0x0051,
} host_link_cmd_t;

#pragma pack(push, 1)
typedef struct {
    uint16_t magic;
    uint8_t version;
    uint8_t reserved;
    uint16_t frame_id;
    uint8_t type;
    uint8_t reserved2;
    uint16_t cmd;
    uint16_t len;
    uint16_t hdr_crc;
} host_link_header_t;

typedef struct {
    int32_t major;
    int32_t minor;
    int32_t patch;
    int32_t build;
} host_link_version_t;

typedef struct {
    uint8_t year;
    uint8_t month;
    uint8_t day;
    uint8_t weekday;
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
} host_link_rtc_tm_t;

typedef struct {
    uint8_t led_id;
    uint8_t duty; /* 0..100 */
} host_link_led_set_t;

typedef struct {
    uint8_t channel;
    uint8_t enable;
} host_link_ch_enable_t;

typedef struct {
    uint8_t channel;
    uint8_t level;
} host_link_alarm_in_evt_t;

typedef struct {
    uint16_t mv;
    int32_t milli;
} host_link_adc_milli_t;

typedef struct {
    int32_t status;
} host_link_status_t;

typedef struct {
    uint32_t baudrate;
    char config[3];
} host_link_rs485_init_t;

typedef struct {
    uint8_t mode; /* 0:all(default), 1:iris, 2:motor */
} host_link_lens_cfg_t;

typedef struct {
    uint8_t iris_state;
    uint8_t zoom_state;
    uint8_t focus_state;
    uint8_t zoom_rz_done;
    uint8_t focus_rz_done;
    int32_t zoom_pos;
    int32_t focus_pos;
} host_link_lens_state_t;

typedef struct {
    uint16_t pps;
    int32_t value; /* run: micro steps; abs: target position */
} host_link_lens_motion_t;

typedef struct {
    int32_t min_pos;
    int32_t max_pos;
} host_link_lens_limit_t;

typedef struct {
    uint16_t zm_pps;
    int32_t zm_micro_steps; /**< micro steps for zoom (0 = skip) */
    uint16_t fs_pps;
    int32_t fs_micro_steps; /**< micro steps for focus (0 = skip) */
} host_link_lens_zf_sync_t;

typedef struct {
    uint16_t target;
} host_link_lens_iris_tgt_t;

typedef struct {
    uint16_t adc;
} host_link_lens_iris_adc_t;

typedef struct {
    uint32_t event;
    int32_t result;
    int32_t zoom_pos;
    int32_t focus_pos;
} host_link_lens_evt_t;

typedef struct {
    /* Same layout as mcu_board_prj/boot ota_package_header_t (see tools/pack_ota.py). */
    uint32_t magic;
    uint32_t build_timestamp;
    uint32_t app_offset;
    uint32_t app_size;
    uint32_t app_crc32;
    char app_version[HOST_LINK_OTA_PACKAGE_VERSION_MAX_LEN];
    uint32_t header_crc32;
} host_link_ota_pkg_hdr_t;

#pragma pack(pop)

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(host_link_header_t) == HOST_LINK_HEADER_WIRE_LEN, "host_link_header_t wire size");
_Static_assert(sizeof(host_link_version_t) == 16, "host_link_version_t");
_Static_assert(sizeof(host_link_rtc_tm_t) == 7, "host_link_rtc_tm_t");
_Static_assert(sizeof(host_link_led_set_t) == 2, "host_link_led_set_t");
_Static_assert(sizeof(host_link_ch_enable_t) == 2, "host_link_ch_enable_t");
_Static_assert(sizeof(host_link_alarm_in_evt_t) == 2, "host_link_alarm_in_evt_t");
_Static_assert(sizeof(host_link_status_t) == 4, "host_link_status_t");
_Static_assert(sizeof(host_link_adc_milli_t) == 6, "host_link_adc_milli_t");
_Static_assert(sizeof(host_link_rs485_init_t) == 7, "host_link_rs485_init_t");
_Static_assert(sizeof(host_link_lens_cfg_t) == 1, "host_link_lens_cfg_t");
_Static_assert(sizeof(host_link_lens_state_t) == 13, "host_link_lens_state_t");
_Static_assert(sizeof(host_link_lens_motion_t) == 6, "host_link_lens_motion_t");
_Static_assert(sizeof(host_link_lens_limit_t) == 8, "host_link_lens_limit_t");
_Static_assert(sizeof(host_link_lens_zf_sync_t) == 12, "host_link_lens_zf_sync_t");
_Static_assert(sizeof(host_link_lens_iris_tgt_t) == 2, "host_link_lens_iris_tgt_t");
_Static_assert(sizeof(host_link_lens_iris_adc_t) == 2, "host_link_lens_iris_adc_t");
_Static_assert(sizeof(host_link_lens_evt_t) == 16, "host_link_lens_evt_t");
_Static_assert(sizeof(host_link_ota_pkg_hdr_t) == 56, "host_link_ota_pkg_hdr_t");
#endif

