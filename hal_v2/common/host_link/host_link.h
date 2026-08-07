/**
 * @file host_link.h
 * @brief MCU <-> Linux host bridge: request/response, event/event_ack, POSIX port.
 *
 * Integration:
 * - One thread reads serial bytes -> host_link_feed() only.
 * - One thread loops host_link_poll().
 * - Application handles REQUEST/EVENT frames in notify callback.
 */
#pragma once

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

void host_link_feed(host_link_handler_t *h, const uint8_t *data, uint16_t len);
void host_link_poll(host_link_handler_t *h);

int host_link_request(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                      void **resp_data, uint16_t *resp_len);
/**
 * @brief Request with a per-call ACK timeout override.
 *
 * This does not affect other requests; use for long-running MCU operations (e.g. OTA flash writes).
 */
int host_link_request_timeout(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                              void **resp_data, uint16_t *resp_len, uint32_t timeout_ms);
int host_link_request_async(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                            host_link_request_done_fn done, void *done_user);

int host_link_response(host_link_handler_t *h, const host_link_frame_t *req_frame, const void *data, uint16_t len);
int host_link_send_event(host_link_handler_t *h, uint16_t cmd, const void *data, uint16_t len);
int host_link_event_ack(host_link_handler_t *h, const host_link_frame_t *event_frame);

void host_link_free(void *ptr);

#ifdef __cplusplus
}
#endif

