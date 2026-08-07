#include "boot_ymodem.h"
#include "ota_module.h"

#include "stm32g0xx_hal_uart_ex.h"

#include <string.h>
#include <stdio.h>

#define YM_SOH 0x01u
#define YM_STX 0x02u
#define YM_EOT 0x04u
#define YM_ACK 0x06u
#define YM_NAK 0x15u
#define YM_CAN 0x18u
#define YM_C   0x43u

#define RX_DMA_BUF      2048u
/* Two Ymodem-1k frames (~2060 B) may arrive in one idle chunk */
#define STREAM_MAX      4096u
#define CHUNK_Q         16u

#define T_DISC_C_MS     200u
#define T_SESSION_MS    3000u
#define NAK_RETRIES     20u

UART_HandleTypeDef *g_boot_debug_uart;

static uint8_t s_u1_rx[RX_DMA_BUF];
static uint8_t s_u2_rx[RX_DMA_BUF];

typedef struct {
    uint8_t  uidx;
    uint16_t len;
    uint8_t  data[RX_DMA_BUF];
} rx_chunk_t;

static rx_chunk_t s_q[CHUNK_Q];
static volatile uint8_t s_q_w;
static volatile uint8_t s_q_r;

static volatile uint8_t s_chunk_overflow;
static volatile uint8_t s_chunk_ovf_uidx;

/* Per-UART RX assembly (must stay separate until link is locked) */
static uint8_t  s_sb[2][STREAM_MAX];
static uint16_t s_sl[2];

static UART_HandleTypeDef *s_active;
static UART_HandleTypeDef *s_debug;
static int s_locked;

static uint32_t s_last_rx_ms;
static uint32_t s_last_c_ms;

static int s_in_filename_phase;
static uint8_t s_expected_seq;
static uint8_t s_last_acked_seq;
static int s_eot_count;

static uint8_t  s_hdr_accum[sizeof(ota_package_header_t)];
static uint16_t s_hdr_accum_len;
static int      s_hdr_done;
static uint32_t s_ota_offset;
static uint32_t s_expected_total;
static int      s_ota_session;

static uint8_t s_can_count;

/* After Xshell tail byte 'O': wire often carries CAN/CAN or stray EOT — not a user cancel. */
static uint8_t s_wire_tail_discard;

static uint8_t s_nak_streak;

/* Any UART: bump rx-idle watchdog if chunk looks like Ymodem (SOH/STX/EOT/CAN), or if this is
 * the active OTA UART (payload blocks usually contain no frame bytes). */
static int rx_buf_has_ymodem_frame(const uint8_t *p, uint16_t len)
{
    for (uint16_t i = 0; i < len; i++) {
        const uint8_t b = p[i];
        if (b == YM_SOH || b == YM_STX || b == YM_EOT || b == YM_CAN) {
            return 1;
        }
    }
    return 0;
}

static void full_reset(const char *why);
static int handle_eot(uint8_t uidx);
static int  rx_sm_enqueue(uint8_t uidx, const uint8_t *p, uint16_t len);
static int  should_listen(uint8_t uidx);
static void start_rx_dma_both(void);
static void start_rx_dma_one(UART_HandleTypeDef *hu, uint8_t *buf);
static void abort_rx_inactive(void);
static void append_stream(uint8_t uidx, const uint8_t *p, uint16_t len);
static void try_consume_frame(uint8_t uidx);
static void drain_chunks(void);
static void lock_ports(uint8_t uidx);

static uint16_t ymodem_crc16(const uint8_t *buf, uint16_t l)
{
    uint16_t crc = 0u;
    for (uint16_t i = 0; i < l; i++) {
        crc ^= (uint16_t)buf[i] << 8u;
        for (int b = 0; b < 8; b++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc <<= 1u;
            }
        }
    }
    return crc;
}

static void tx_byte(UART_HandleTypeDef *hu, uint8_t b)
{
    if (!hu) {
        return;
    }
    (void)HAL_UART_Transmit(hu, &b, 1u, 50u);
}

static void send_ack(void)
{
    tx_byte(s_active, YM_ACK);
}

static void send_nak(void)
{
    tx_byte(s_active, YM_NAK);
}

/* After ACK: wait UART TC so last bit leaves the wire before more bytes (Xshell Ymodem end). */
static void uart_wait_tx_line_done(UART_HandleTypeDef *hu)
{
    if (!hu) {
        return;
    }
    uint32_t t0 = HAL_GetTick();
    while (__HAL_UART_GET_FLAG(hu, UART_FLAG_TC) == RESET) {
        if ((uint32_t)(HAL_GetTick() - t0) > 40u) {
            break;
        }
    }
    HAL_Delay(10);
}

/* Ymodem abort: NAK means retry block; CAN burst tells host to stop the transfer. */
#define YM_CANCEL_BURST 8u
static void send_ymodem_cancel_on(UART_HandleTypeDef *hu)
{
    if (!hu) {
        return;
    }
    for (uint8_t n = 0u; n < YM_CANCEL_BURST; n++) {
        tx_byte(hu, YM_CAN);
    }
    uart_wait_tx_line_done(hu);
}

static UART_HandleTypeDef *uart_from_uidx(uint8_t uidx)
{
    return (uidx == 0u) ? &huart1 : &huart2;
}

static void send_ymodem_cancel(void)
{
    send_ymodem_cancel_on(s_active);
}

/* Xshell YMODEM: after 2nd-EOT ACK, host expects 'C' then null closing SOH, then extra 'O'
 * (https://blog.csdn.net/weixin_51426754/article/details/140680264) */

static void send_c_discovery(void)
{
    uint8_t b = YM_C;
    if (!s_locked) {
        (void)HAL_UART_Transmit(&huart1, &b, 1u, 50u);
        (void)HAL_UART_Transmit(&huart2, &b, 1u, 50u);
    } else if (s_active) {
        (void)HAL_UART_Transmit(s_active, &b, 1u, 50u);
    }
}

static void abort_rx_inactive(void)
{
    if (s_active == &huart1) {
        (void)HAL_UART_AbortReceive(&huart2);
    } else if (s_active == &huart2) {
        (void)HAL_UART_AbortReceive(&huart1);
    }
}

static void start_rx_dma_one(UART_HandleTypeDef *hu, uint8_t *buf)
{
    if (HAL_UARTEx_ReceiveToIdle_DMA(hu, buf, (uint16_t)RX_DMA_BUF) != HAL_OK) {
        (void)HAL_UART_AbortReceive(hu);
        (void)HAL_UARTEx_ReceiveToIdle_DMA(hu, buf, (uint16_t)RX_DMA_BUF);
    }
}

static void start_rx_dma_both(void)
{
    s_locked = 0;
    s_active = NULL;
    s_debug = &huart1;
    g_boot_debug_uart = s_debug;
    (void)HAL_UART_AbortReceive(&huart1);
    (void)HAL_UART_AbortReceive(&huart2);
    start_rx_dma_one(&huart1, s_u1_rx);
    start_rx_dma_one(&huart2, s_u2_rx);
}

static int should_listen(uint8_t uidx)
{
    if (!s_locked) {
        return 1;
    }
    if (uidx == 0u) {
        return (s_active == &huart1);
    }
    return (s_active == &huart2);
}

static int rx_sm_enqueue(uint8_t uidx, const uint8_t *p, uint16_t len)
{
    uint8_t nw = (uint8_t)((s_q_w + 1u) % CHUNK_Q);
    if (nw == s_q_r) {
        return -1;
    }
    s_q[s_q_w].uidx = uidx;
    s_q[s_q_w].len = len;
    if (len > RX_DMA_BUF) {
        len = (uint16_t)RX_DMA_BUF;
    }
    memcpy(s_q[s_q_w].data, p, len);
    s_q_w = nw;
    return 0;
}

static void ota_fail_abort(void)
{
    if (s_ota_session) {
        (void)ota_module_ota_download_abort();
        s_ota_session = 0;
    }
}

static void session_vars_clear(void)
{
    s_sl[0] = 0;
    s_sl[1] = 0;
    memset(s_sb, 0, sizeof(s_sb));
    s_in_filename_phase = 0;
    s_expected_seq = 0;
    s_last_acked_seq = 255u;
    s_eot_count = 0;
    memset(s_hdr_accum, 0, sizeof(s_hdr_accum));
    s_hdr_accum_len = 0;
    s_hdr_done = 0;
    s_ota_offset = 0;
    s_expected_total = 0;
    s_can_count = 0;
    s_wire_tail_discard = 0u;
    s_nak_streak = 0;
    ota_fail_abort();
}

static void full_reset(const char *why)
{
    printf("[YM] reset: %s\r\n", why ? why : "?");
    ota_fail_abort();
    session_vars_clear();
    s_locked = 0;
    s_active = NULL;
    g_boot_debug_uart = &huart1;
    s_last_c_ms = HAL_GetTick();
    s_last_rx_ms = s_last_c_ms;
    s_q_r = 0;
    s_q_w = 0;
    s_chunk_overflow = 0;
    s_chunk_ovf_uidx = 0u;
    start_rx_dma_both();
}

static void lock_ports(uint8_t uidx)
{
    s_locked = 1;
    if (uidx == 0u) {
        s_active = &huart1;
        s_debug = &huart2;
        s_sl[1] = 0;
    } else {
        s_active = &huart2;
        s_debug = &huart1;
        s_sl[0] = 0;
    }
    g_boot_debug_uart = s_debug;
    s_expected_seq = 0;
    s_in_filename_phase = 1;
    s_eot_count = 0;
    abort_rx_inactive();
}

static void append_file_payload(const uint8_t *p, uint16_t len)
{
    uint16_t i = 0;
    while (i < len) {
        if (!s_hdr_done) {
            uint16_t need = (uint16_t)(sizeof(ota_package_header_t) - s_hdr_accum_len);
            uint16_t take = (uint16_t)(len - i < need ? (len - i) : need);
            memcpy(s_hdr_accum + s_hdr_accum_len, p + i, take);
            s_hdr_accum_len = (uint16_t)(s_hdr_accum_len + take);
            i = (uint16_t)(i + take);
            if (s_hdr_accum_len >= sizeof(ota_package_header_t)) {
                ota_package_header_t hdr;
                memcpy(&hdr, s_hdr_accum, sizeof(hdr));
                if (ota_module_ota_download_start(&hdr) != OTA_MODULE_ERR_OK) {
                    s_nak_streak = NAK_RETRIES;
                    return;
                }
                s_ota_session = 1;
                if (ota_module_ota_download(0u, s_hdr_accum, (uint32_t)sizeof(ota_package_header_t)) != OTA_MODULE_ERR_OK) {
                    s_nak_streak = NAK_RETRIES;
                    return;
                }
                s_hdr_done = 1;
                s_ota_offset = (uint32_t)sizeof(ota_package_header_t);
                s_expected_total = hdr.app_offset + hdr.app_size;
            }
        } else {
            uint32_t remain;
            if (s_expected_total >= s_ota_offset) {
                remain = s_expected_total - s_ota_offset;
            } else {
                remain = 0u;
            }
            if (remain == 0u) {
                break;
            }
            uint16_t chunk = (uint16_t)(len - i);
            if (chunk > remain) {
                chunk = (uint16_t)remain;
            }
            if (ota_module_ota_download(s_ota_offset, p + i, chunk) != OTA_MODULE_ERR_OK) {
                s_nak_streak = NAK_RETRIES;
                return;
            }
            s_ota_offset += chunk;
            i = (uint16_t)(i + chunk);
        }
    }
}

static int handle_eot(uint8_t uidx)
{
    if (!s_locked || !s_active) {
        return 0;
    }

    if (s_eot_count == 0) {
        send_nak();
        s_eot_count = 1;
        return 0;
    }
    if (s_eot_count >= 2) {
        return 0;
    }
    s_eot_count = 2;
    /* 2nd EOT: ACK -> C (request closing SOH) -> drain/ACK null frame -> O (Xshell). */
    send_ack();
    uart_wait_tx_line_done(s_active);
    send_c_discovery();
    HAL_Delay(15);
    drain_chunks();
    try_consume_frame(uidx);
    tx_byte(s_active, (uint8_t)'O');
    uart_wait_tx_line_done(s_active);
    /* Xshell / many hosts send CAN CAN after 'O'; must not call full_reset("can") here. */
    s_can_count = 0u;
    s_wire_tail_discard = 1u;
    drain_chunks();
    try_consume_frame(uidx);
    if (s_ota_session && s_hdr_done) {
        int fin = ota_module_ota_download_finish();
        if (fin == OTA_MODULE_ERR_OK) {
            printf("OTA OK, boot app...\r\n");
            (void)ota_module_set_boot_ymodem_flag(0u);
            /* finish() cleared g_dl; clear boot flag before full_reset so ota_fail_abort() does not
             * call ota_download_abort() (slot is no longer DOWNLOADING -> spurious LOGE). */
            s_ota_session = 0;
            (void)ota_module_boot_preprocess();
            /* no return if jump succeeded */
        } else {
            printf("ota finish err %d\r\n", fin);
            (void)ota_module_ota_download_abort();
            s_ota_session = 0;
        }
    }
    full_reset("eot");
    return 1;
}

static void handle_data_block(uint8_t seq, const uint8_t *payload, uint16_t paylen)
{
    if (seq != s_expected_seq) {
        if (seq == s_last_acked_seq) {
            send_ack();
            return;
        }
        send_nak();
        if (++s_nak_streak > NAK_RETRIES) {
            send_ymodem_cancel();
            full_reset("seq");
        }
        return;
    }

    if (s_in_filename_phase) {
        (void)payload;
        (void)paylen;
        s_in_filename_phase = 0;
        s_expected_seq = 1u;
        s_last_acked_seq = seq;
        send_ack();
        s_nak_streak = 0;
        return;
    }

    append_file_payload(payload, paylen);
    if (s_nak_streak >= NAK_RETRIES) {
        /* Header / flash write rejected: do not NAK (host would only resend same bad data). */
        send_ymodem_cancel();
        full_reset("ota wr");
        return;
    }

    s_last_acked_seq = seq;
    s_expected_seq = (uint8_t)(seq + 1u);
    send_ack();
    s_nak_streak = 0;
}

static void try_consume_frame(uint8_t uidx)
{
    uint8_t *buf = s_sb[uidx];
    uint16_t *plen = &s_sl[uidx];

    while ((*plen) > 0u) {
        uint16_t i = 0;
        while (i < *plen && buf[i] != YM_SOH && buf[i] != YM_STX && buf[i] != YM_EOT && buf[i] != YM_CAN) {
            i++;
        }
        if (i > 0u) {
            memmove(buf, buf + i, *plen - i);
            *plen = (uint16_t)(*plen - i);
        }
        if ((*plen) == 0u) {
            return;
        }

        if (s_locked && (((uidx == 0u) && (s_active != &huart1)) || ((uidx == 1u) && (s_active != &huart2)))) {
            *plen = 0;
            return;
        }

        if (buf[0] == YM_CAN) {
            if (s_wire_tail_discard) {
                memmove(buf, buf + 1u, *plen - 1u);
                (*plen)--;
                continue;
            }
            s_can_count++;
            memmove(buf, buf + 1u, *plen - 1u);
            (*plen)--;
            if (s_can_count >= 2u) {
                full_reset("can");
            }
            continue;
        }
        s_can_count = 0;

        if (buf[0] == YM_EOT) {
            memmove(buf, buf + 1u, *plen - 1u);
            (*plen)--;
            if (s_wire_tail_discard) {
                continue;
            }
            (void)handle_eot(uidx);
            continue;
        }

        if (buf[0] != YM_SOH && buf[0] != YM_STX) {
            return;
        }

        uint8_t st = buf[0];
        uint16_t dlen = (st == YM_SOH) ? 128u : 1024u;
        uint16_t frlen = (uint16_t)(1u + 1u + 1u + dlen + 2u);
        if ((*plen) < frlen) {
            return;
        }

        uint8_t seq = buf[1];
        uint8_t nseq = buf[2];
        if ((uint8_t)(seq + nseq) != 255u) {
            memmove(buf, buf + 1u, *plen - 1u);
            (*plen)--;
            if (s_locked) {
                send_nak();
            }
            continue;
        }

        const uint8_t *pay = &buf[3];
        uint16_t crc_rx = (uint16_t)(((uint16_t)buf[3u + dlen] << 8) | buf[3u + dlen + 1u]);
        uint16_t crc_ok = ymodem_crc16(pay, dlen);
        if (crc_rx != crc_ok) {
            memmove(buf, buf + 1u, *plen - 1u);
            (*plen)--;
            if (s_locked) {
                send_nak();
                if (++s_nak_streak > NAK_RETRIES) {
                    send_ymodem_cancel();
                    full_reset("crc");
                }
            }
            continue;
        }

        /* YMODEM batch end: SOH #0 + null pathname only *after* 2nd EOT (s_eot_count>=2).
         * Do not use s_hdr_done alone: SOH file blocks wrap seq 0..255; payload[0]==0 is common
         * on a real data block #256 — mis-ACK here drops 128B and leads to full_reset("seq"). */
        if (st == YM_SOH && seq == 0u && pay[0] == 0u && s_hdr_done && s_eot_count >= 2) {
            if (!s_locked) {
                lock_ports(uidx);
            }
            send_ack();
            memmove(buf, buf + frlen, *plen - frlen);
            *plen = (uint16_t)(*plen - frlen);
            continue;
        }

        if (!s_locked) {
            lock_ports(uidx);
        }

        handle_data_block(seq, pay, dlen);
        memmove(buf, buf + frlen, *plen - frlen);
        *plen = (uint16_t)(*plen - frlen);
    }
}

static void append_stream(uint8_t uidx, const uint8_t *p, uint16_t len)
{
    if ((uint32_t)s_sl[uidx] + len > STREAM_MAX) {
        UART_HandleTypeDef *hu_tx = s_active ? s_active : uart_from_uidx(uidx);
        send_ymodem_cancel_on(hu_tx);
        full_reset("stream ovf");
        return;
    }
    memcpy(s_sb[uidx] + s_sl[uidx], p, len);
    s_sl[uidx] = (uint16_t)(s_sl[uidx] + len);
    try_consume_frame(uidx);
}

static void drain_chunks(void)
{
    if (s_chunk_overflow) {
        UART_HandleTypeDef *hu_tx = s_active ? s_active : uart_from_uidx(s_chunk_ovf_uidx);
        send_ymodem_cancel_on(hu_tx);
        full_reset("chunk ovf");
        return;
    }

    while (s_q_r != s_q_w) {
        rx_chunk_t ch = s_q[s_q_r];
        s_q_r = (uint8_t)((s_q_r + 1u) % CHUNK_Q);
        if (!should_listen(ch.uidx)) {
            continue;
        }
        append_stream(ch.uidx, ch.data, ch.len);
    }
}

void boot_ymodem_init(void)
{
    g_boot_debug_uart = &huart1;
    session_vars_clear();
    s_q_r = s_q_w = 0;
    s_chunk_overflow = 0;
    s_chunk_ovf_uidx = 0u;
    s_last_c_ms = HAL_GetTick();
    s_last_rx_ms = s_last_c_ms;
    start_rx_dma_both();
    send_c_discovery();
}

int boot_ymodem_probe(uint32_t discovery_rounds)
{
    if (discovery_rounds == 0u) {
        return -1;
    }
    const uint32_t window_ms = (uint32_t)BOOT_YM_PROBE_WINDOW_MS;
    for (uint32_t r = 0u; r < discovery_rounds; r++) {
        send_c_discovery();
        s_last_c_ms = HAL_GetTick();

        uint32_t t0 = HAL_GetTick();
        while ((uint32_t)(HAL_GetTick() - t0) < window_ms) {
            drain_chunks();
            if (s_locked) {
                return 0;
            }
            if (s_sl[0] > 0u || s_sl[1] > 0u) {
                return 0;
            }
            HAL_Delay(1);
        }
    }
    /* Avoid immediate rx-idle full_reset in poll() after a long silent probe. */
    uint32_t now = HAL_GetTick();
    s_last_rx_ms = now;
    s_last_c_ms = now;
    return -1;
}

void boot_ymodem_poll(void)
{
    drain_chunks();

    /* Read tick after drain: flash + parsing can take many ms; UART ISR may have
     * advanced s_last_rx_ms while drain_chunks() ran. Using a stale "now" from poll
     * entry would make (now - s_last_rx_ms) unsigned-wrap and falsely trigger rx idle. */
    uint32_t now = HAL_GetTick();

    if ((uint32_t)(now - s_last_c_ms) >= T_DISC_C_MS) {
        if (!s_locked) {
            send_c_discovery();
        }
        s_last_c_ms = now;
    }

    {
        const int32_t idle_dt = (int32_t)(now - s_last_rx_ms);
        if (idle_dt >= 0 && (uint32_t)idle_dt >= T_SESSION_MS) {
            if (s_locked) {
                send_ymodem_cancel();
            }
            /* No UART payload for T_SESSION_MS: if record shows ACTIVE app, cold-reset so next
             * boot runs boot_preprocess and enters app (avoids endless rx-idle / C spam). */
            if (ota_module_has_active_app_region()) {
                printf("ymodem idle, reset\r\n");
                NVIC_SystemReset();
            }
            full_reset("rx idle");
        }
    }
}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t Size)
{
    uint8_t uidx = (huart == &huart1) ? 0u : 1u;
    uint8_t *buf = (uidx == 0u) ? s_u1_rx : s_u2_rx;

    if (Size > RX_DMA_BUF) {
        Size = RX_DMA_BUF;
    }

    if (Size > 0u && (rx_buf_has_ymodem_frame(buf, Size) || should_listen(uidx))) {
        s_last_rx_ms = HAL_GetTick();
    }

    if (!should_listen(uidx)) {
        start_rx_dma_one(huart, buf);
        return;
    }

    if (rx_sm_enqueue(uidx, buf, Size) != 0) {
        s_chunk_ovf_uidx = uidx;
        s_chunk_overflow = 1u;
        (void)HAL_UART_AbortReceive(huart);
        return;
    }

    start_rx_dma_one(huart, buf);
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    __HAL_UART_CLEAR_FLAG(huart, UART_CLEAR_OREF | UART_CLEAR_NEF | UART_CLEAR_FEF | UART_CLEAR_PEF);

    uint8_t uidx = (huart == &huart1) ? 0u : 1u;
    uint8_t *buf = (uidx == 0u) ? s_u1_rx : s_u2_rx;

    start_rx_dma_one(huart, buf);
}
