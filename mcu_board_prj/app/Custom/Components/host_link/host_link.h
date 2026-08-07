/**
 * @file host_link.h
 * @brief MCU <-> Linux host bridge: request/response, event/event_ack, dual-port (FreeRTOS/POSIX).
 *
 * Integration (non-blocking events while long REQUEST handling):
 * - One task/thread: read transport -> host_link_feed() only.
 * - One task/thread: loop host_link_poll() (short timeout inside).
 * - In notify_cb for REQUEST: copy data and enqueue to a worker; return quickly; call host_link_response() from worker.
 * - Avoid calling host_link_request() from inside notify_cb on the same handler (deadlock risk); use host_link_request_async() or defer to another context.
 * - host_link_request_async() timeouts are reported via the done callback when host_link_poll() runs after HOST_LINK_CFG_ACK_TIMEOUT_MS.
 *
 * UART / serial link (MCU <-> Linux on same wire):
 * - Agree on line format with the host (typically 8N1; enable HW flow control only if both sides wire RTS/CTS).
 * - MCU: implement host_link_send_fn to push one full frame with HAL_UART_Transmit (or DMA + completion), blocking or with timeout matching HOST_LINK_CFG_SEND_TIMEOUT_MS.
 * - MCU RX: in a dedicated task, obtain bytes from UART (e.g. circular DMA + CNDTR read index, or ReceiveToIdle + idle callback) and call host_link_feed() with each chunk; do not parse application data in the ISR beyond storing bytes.
 * - Linux: open the serial device (e.g. /dev/ttyUSB0, /dev/ttyS0) with termios (baud, 8N1); one thread blocks on read(2) and calls host_link_feed(); host_link_send_fn uses write(2) (handle partial writes in a loop).
 * - host_link is a byte stream protocol: no extra framing beyond its magic header; keep the UART dedicated to this link or add a demuxer if the port is shared.
 *
 * Override before #include "host_link.h":
 *   HOST_LINK_CFG_MAX_PAYLOAD, HOST_LINK_CFG_NOTIFY_DEPTH, HOST_LINK_CFG_ACK_SLOTS,
 *   HOST_LINK_CFG_ASYNC_MAX, HOST_LINK_CFG_*_TIMEOUT_MS, HOST_LINK_CFG_*_RETRY
 */
#ifndef HOST_LINK_H
#define HOST_LINK_H

#include "host_link_proto.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef HOST_LINK_CFG_MAX_PAYLOAD
#define HOST_LINK_CFG_MAX_PAYLOAD 512u
#endif
#ifndef HOST_LINK_CFG_NOTIFY_DEPTH
#define HOST_LINK_CFG_NOTIFY_DEPTH 16u
#endif
#ifndef HOST_LINK_CFG_ACK_SLOTS
#define HOST_LINK_CFG_ACK_SLOTS 8u
#endif
#ifndef HOST_LINK_CFG_ASYNC_MAX
#define HOST_LINK_CFG_ASYNC_MAX 4u
#endif
#ifndef HOST_LINK_CFG_SEND_TIMEOUT_MS
#define HOST_LINK_CFG_SEND_TIMEOUT_MS 100u
#endif
#ifndef HOST_LINK_CFG_ACK_WAIT_SLICE_MS
#define HOST_LINK_CFG_ACK_WAIT_SLICE_MS 20u
#endif
#ifndef HOST_LINK_CFG_ACK_TIMEOUT_MS
#define HOST_LINK_CFG_ACK_TIMEOUT_MS 500u
#endif
#ifndef HOST_LINK_CFG_POLL_WAIT_MS
#define HOST_LINK_CFG_POLL_WAIT_MS 20u
#endif
#ifndef HOST_LINK_CFG_REQUEST_RETRY
#define HOST_LINK_CFG_REQUEST_RETRY 3u
#endif

typedef struct host_link_handler host_link_handler_t;

typedef struct {
    host_link_header_t header;
    uint8_t *payload;
    uint16_t payload_crc;
    uint8_t is_valid;
} host_link_frame_t;

typedef int (*host_link_send_fn)(void *user_ctx, const uint8_t *buf, uint16_t len, uint32_t timeout_ms);
typedef void (*host_link_notify_fn)(void *user_ctx, host_link_handler_t *h, host_link_frame_t *frame);
typedef void (*host_link_request_done_fn)(void *user, int err, uint8_t *data, uint16_t len);

host_link_handler_t *host_link_init(host_link_send_fn send_fn, host_link_notify_fn notify_fn, void *user_ctx);
void host_link_deinit(host_link_handler_t *h);

/** Push raw bytes from transport (call from RX thread only; not ISR unless port mutex is IRQ-safe). */
void host_link_feed(host_link_handler_t *h, const uint8_t *data, uint16_t len);

/**
 * Dispatch pending REQUEST/EVENT frames to notify_fn. Frees frame payload after callback returns.
 * Blocks up to HOST_LINK_CFG_POLL_WAIT_MS waiting for work when idle.
 */
void host_link_poll(host_link_handler_t *h);

/** Synchronous request; blocks calling thread until RESPONSE or timeout. */
int host_link_request(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                      void **resp_data, uint16_t *resp_len);

/** Async request; @a done receives malloc'd response payload on success — use host_link_free() on @a data when done. */
int host_link_request_async(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                            host_link_request_done_fn done, void *done_user);

int host_link_response(host_link_handler_t *h, const host_link_frame_t *req_frame, const void *data, uint16_t len);
int host_link_send_event(host_link_handler_t *h, uint16_t cmd, const void *data, uint16_t len);
int host_link_event_ack(host_link_handler_t *h, const host_link_frame_t *event_frame);

void host_link_free(void *ptr);

#ifdef __cplusplus
}
#endif

#endif /* HOST_LINK_H */
