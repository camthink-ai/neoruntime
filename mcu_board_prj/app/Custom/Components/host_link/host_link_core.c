#include "host_link.h"
#include "host_link_port.h"
#include <string.h>

enum {
    PS_SYNC0 = 0,
    PS_SYNC1,
    PS_HDR,
    PS_PAYLOAD,
};

struct host_link_handler {
    uint8_t ready;
    host_link_send_fn send_fn;
    host_link_notify_fn notify_cb;
    void *user_ctx;

    host_link_mutex_t lock;
    host_link_sem_t ack_sem;
    host_link_sem_t notify_sem;

    uint16_t next_frame_id;

    uint8_t parse_state;
    uint16_t parse_got;
    uint8_t hdr_raw[HOST_LINK_HEADER_WIRE_LEN];
    host_link_header_t pend_hdr;
    uint8_t *pend_payload;
    uint16_t pend_pay_need;

    host_link_frame_t notify_slot[HOST_LINK_CFG_NOTIFY_DEPTH];
    host_link_frame_t ack_slot[HOST_LINK_CFG_ACK_SLOTS];
    uint32_t ack_tick[HOST_LINK_CFG_ACK_SLOTS];

    struct {
        uint8_t used;
        uint16_t id;
        uint16_t cmd;
        uint32_t started_ms;
        host_link_request_done_fn cb;
        void *cb_user;
    } async_slot[HOST_LINK_CFG_ASYNC_MAX];
};

static uint16_t host_link_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = 0xFFFFu;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)data[i] << 8;
        for (uint8_t j = 0; j < 8u; j++) {
            if (crc & 0x8000u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

void host_link_free(void *ptr)
{
    host_link_port_free(ptr);
}

static int hdr_crc_ok(const host_link_header_t *h)
{
    uint16_t c = host_link_crc16((const uint8_t *)h, HOST_LINK_HDR_CRC_LEN);
    return (c == h->hdr_crc) ? HOST_LINK_OK : HOST_LINK_ERR_CRC;
}

static int payload_crc_ok(const uint8_t *pay, uint16_t len, uint16_t expect_crc)
{
    uint16_t c = host_link_crc16((len > 0u) ? pay : (const uint8_t *)"", len);
    return (c == expect_crc) ? HOST_LINK_OK : HOST_LINK_ERR_CRC;
}

static void parse_reset(struct host_link_handler *h)
{
    h->parse_state = PS_SYNC0;
    h->parse_got = 0;
    if (h->pend_payload != NULL) {
        host_link_port_free(h->pend_payload);
        h->pend_payload = NULL;
    }
    h->pend_pay_need = 0;
    memset(h->hdr_raw, 0, sizeof(h->hdr_raw));
}

static int find_empty_notify(struct host_link_handler *h)
{
    for (uint16_t i = 0; i < HOST_LINK_CFG_NOTIFY_DEPTH; i++) {
        if (!h->notify_slot[i].is_valid) {
            return (int)i;
        }
    }
    return HOST_LINK_ERR_NO_SLOT;
}

static int find_empty_ack(struct host_link_handler *h)
{
    for (uint16_t i = 0; i < HOST_LINK_CFG_ACK_SLOTS; i++) {
        if (!h->ack_slot[i].is_valid) {
            return (int)i;
        }
    }
    return HOST_LINK_ERR_NO_SLOT;
}

static void free_frame_payload(host_link_frame_t *f)
{
    if (f->payload != NULL) {
        host_link_port_free(f->payload);
        f->payload = NULL;
    }
    f->is_valid = 0;
}

/**
 * Caller must hold h->lock. For async completion, unlocks before @a done_cb, re-locks after.
 */
static int add_ack_frame_locked(struct host_link_handler *h, host_link_frame_t *f)
{
    if (f->header.type == HOST_LINK_TYPE_RESPONSE) {
        for (uint16_t i = 0; i < HOST_LINK_CFG_ASYNC_MAX; i++) {
            if (h->async_slot[i].used && h->async_slot[i].id == f->header.frame_id
                && h->async_slot[i].cmd == f->header.cmd) {
                host_link_request_done_fn cb = h->async_slot[i].cb;
                void *u = h->async_slot[i].cb_user;
                h->async_slot[i].used = 0;
                uint8_t *pay = f->payload;
                uint16_t plen = f->header.len;
                f->payload = NULL;
                f->is_valid = 0;
                host_link_port_mutex_unlock(h->lock);
                if (cb != NULL) {
                    cb(u, HOST_LINK_OK, pay, plen);
                }
                host_link_port_mutex_lock(h->lock);
                return HOST_LINK_OK;
            }
        }
    }

    int slot = find_empty_ack(h);
    if (slot < 0) {
        return HOST_LINK_ERR_NO_SLOT;
    }
    memcpy(&h->ack_slot[slot], f, sizeof(host_link_frame_t));
    h->ack_slot[slot].is_valid = 1;
    h->ack_tick[slot] = host_link_port_tick_ms();
    f->payload = NULL;
    f->is_valid = 0;
    host_link_port_sem_post(h->ack_sem);
    return HOST_LINK_OK;
}

static int add_notify_frame_locked(struct host_link_handler *h, host_link_frame_t *f)
{
    int slot = find_empty_notify(h);
    if (slot < 0) {
        return HOST_LINK_ERR_NO_SLOT;
    }
    memcpy(&h->notify_slot[slot], f, sizeof(host_link_frame_t));
    h->notify_slot[slot].is_valid = 1;
    f->payload = NULL;
    f->is_valid = 0;
    host_link_port_sem_post(h->notify_sem);
    return HOST_LINK_OK;
}

static int deal_one_frame(struct host_link_handler *h, host_link_frame_t *f)
{
    int e = hdr_crc_ok(&f->header);
    if (e != HOST_LINK_OK) {
        return e;
    }
    if (f->header.magic != HOST_LINK_MAGIC || f->header.version != HOST_LINK_VERSION_CURRENT) {
        return HOST_LINK_ERR_PROTO;
    }
    e = payload_crc_ok(f->payload, f->header.len, f->payload_crc);
    if (e != HOST_LINK_OK) {
        return e;
    }

    host_link_port_mutex_lock(h->lock);
    if (!h->ready) {
        host_link_port_mutex_unlock(h->lock);
        return HOST_LINK_ERR_INVALID_STATE;
    }

    int ret;
    switch (f->header.type) {
    case HOST_LINK_TYPE_REQUEST:
    case HOST_LINK_TYPE_EVENT:
        ret = add_notify_frame_locked(h, f);
        break;
    case HOST_LINK_TYPE_RESPONSE:
    case HOST_LINK_TYPE_EVENT_ACK:
        ret = add_ack_frame_locked(h, f);
        break;
    default:
        ret = HOST_LINK_ERR_PROTO;
        break;
    }
    host_link_port_mutex_unlock(h->lock);
    return ret;
}

static void feed_finish_payload(struct host_link_handler *h)
{
    uint8_t *raw = h->pend_payload;
    uint16_t l = h->pend_hdr.len;
    uint16_t wire_crc = 0;
    memcpy(&wire_crc, raw + l, sizeof(uint16_t));

    if (payload_crc_ok(raw, l, wire_crc) != HOST_LINK_OK) {
        host_link_port_free(raw);
        h->pend_payload = NULL;
        parse_reset(h);
        return;
    }

    host_link_frame_t fr = {0};
    memcpy(&fr.header, &h->pend_hdr, sizeof(fr.header));
    fr.payload_crc = wire_crc;
    fr.is_valid = 1;
    if (l > 0u) {
        fr.payload = (uint8_t *)host_link_port_malloc(l);
        if (fr.payload == NULL) {
            host_link_port_free(raw);
            h->pend_payload = NULL;
            parse_reset(h);
            return;
        }
        memcpy(fr.payload, raw, l);
    } else {
        fr.payload = NULL;
    }

    host_link_port_free(raw);
    h->pend_payload = NULL;

    int r = deal_one_frame(h, &fr);
    if (r != HOST_LINK_OK) {
        free_frame_payload(&fr);
    }
    parse_reset(h);
}

static void feed_byte(struct host_link_handler *h, uint8_t b)
{
    switch (h->parse_state) {
    case PS_SYNC0:
        if (b == (uint8_t)(HOST_LINK_MAGIC & 0xFFu)) {
            h->parse_state = PS_SYNC1;
        }
        break;
    case PS_SYNC1:
        if (b == (uint8_t)((HOST_LINK_MAGIC >> 8) & 0xFFu)) {
            h->hdr_raw[0] = (uint8_t)(HOST_LINK_MAGIC & 0xFFu);
            h->hdr_raw[1] = (uint8_t)((HOST_LINK_MAGIC >> 8) & 0xFFu);
            h->parse_got = 2u;
            h->parse_state = PS_HDR;
        } else if (b == (uint8_t)(HOST_LINK_MAGIC & 0xFFu)) {
            h->parse_state = PS_SYNC1;
        } else {
            h->parse_state = PS_SYNC0;
        }
        break;
    case PS_HDR:
        if (h->parse_got < HOST_LINK_HEADER_WIRE_LEN) {
            h->hdr_raw[h->parse_got++] = b;
        }
        if (h->parse_got == HOST_LINK_HEADER_WIRE_LEN) {
            memcpy(&h->pend_hdr, h->hdr_raw, sizeof(h->pend_hdr));
            if (h->pend_hdr.len > HOST_LINK_CFG_MAX_PAYLOAD) {
                parse_reset(h);
                return;
            }
            h->pend_pay_need = (uint16_t)(h->pend_hdr.len + 2u);
            if (h->pend_pay_need == 2u && h->pend_hdr.len == 0u) {
                h->pend_payload = (uint8_t *)host_link_port_malloc(2u);
                if (h->pend_payload == NULL) {
                    parse_reset(h);
                    return;
                }
                h->parse_got = 0;
                h->parse_state = PS_PAYLOAD;
            } else if (h->pend_hdr.len > 0u) {
                h->pend_payload = (uint8_t *)host_link_port_malloc(h->pend_pay_need);
                if (h->pend_payload == NULL) {
                    parse_reset(h);
                    return;
                }
                h->parse_got = 0;
                h->parse_state = PS_PAYLOAD;
            } else {
                parse_reset(h);
            }
        }
        break;
    case PS_PAYLOAD:
        if (h->pend_payload != NULL && h->parse_got < h->pend_pay_need) {
            h->pend_payload[h->parse_got++] = b;
        }
        if (h->parse_got >= h->pend_pay_need) {
            feed_finish_payload(h);
        }
        break;
    default:
        parse_reset(h);
        break;
    }
}

void host_link_feed(struct host_link_handler *h, const uint8_t *data, uint16_t len)
{
    if (h == NULL || data == NULL) {
        return;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (!h->ready) {
            return;
        }
        feed_byte(h, data[i]);
    }
}

static int send_wire(struct host_link_handler *h, host_link_header_t *hdr, const uint8_t *payload, uint16_t pay_len)
{
    hdr->hdr_crc = host_link_crc16((const uint8_t *)hdr, HOST_LINK_HDR_CRC_LEN);
    uint16_t pay_crc = host_link_crc16((pay_len > 0u && payload != NULL) ? payload : (const uint8_t *)"", pay_len);

    uint16_t total = HOST_LINK_HEADER_WIRE_LEN + pay_len + 2u;
    uint8_t *buf = (uint8_t *)host_link_port_malloc(total);
    if (buf == NULL) {
        return HOST_LINK_ERR_NO_MEM;
    }
    memcpy(buf, hdr, HOST_LINK_HEADER_WIRE_LEN);
    if (pay_len > 0u && payload != NULL) {
        memcpy(buf + HOST_LINK_HEADER_WIRE_LEN, payload, pay_len);
    }
    memcpy(buf + HOST_LINK_HEADER_WIRE_LEN + pay_len, &pay_crc, sizeof(uint16_t));

    int sr = h->send_fn(h->user_ctx, buf, total, HOST_LINK_CFG_SEND_TIMEOUT_MS);
    host_link_port_free(buf);
    return (sr == 0) ? HOST_LINK_OK : HOST_LINK_ERR_SEND;
}

static int wait_for_ack(struct host_link_handler *h, host_link_frame_type_t typ, uint16_t cmd, uint16_t frame_id,
                        host_link_frame_t *out)
{
    uint32_t t0 = host_link_port_tick_ms();
    for (;;) {
        if (!h->ready) {
            return HOST_LINK_ERR_INVALID_STATE;
        }
        host_link_port_mutex_lock(h->lock);
        for (uint16_t i = 0; i < HOST_LINK_CFG_ACK_SLOTS; i++) {
            if (h->ack_slot[i].is_valid && h->ack_slot[i].header.frame_id == frame_id
                && h->ack_slot[i].header.cmd == cmd && h->ack_slot[i].header.type == typ) {
                memcpy(out, &h->ack_slot[i], sizeof(host_link_frame_t));
                h->ack_slot[i].payload = NULL;
                h->ack_slot[i].is_valid = 0;
                host_link_port_mutex_unlock(h->lock);
                return HOST_LINK_OK;
            }
        }
        host_link_port_mutex_unlock(h->lock);

        uint32_t now = host_link_port_tick_ms();
        if (host_link_port_tick_diff_ms(t0, now) >= HOST_LINK_CFG_ACK_TIMEOUT_MS) {
            return HOST_LINK_ERR_TIMEOUT;
        }
        host_link_port_sem_wait(h->ack_sem, HOST_LINK_CFG_ACK_WAIT_SLICE_MS);
    }
}

host_link_handler_t *host_link_init(host_link_send_fn send_fn, host_link_notify_fn notify_fn, void *user_ctx)
{
    if (send_fn == NULL || notify_fn == NULL) {
        return NULL;
    }
    struct host_link_handler *h = (struct host_link_handler *)host_link_port_malloc(sizeof(*h));
    if (h == NULL) {
        return NULL;
    }
    memset(h, 0, sizeof(*h));
    h->send_fn = send_fn;
    h->notify_cb = notify_fn;
    h->user_ctx = user_ctx;
    h->lock = host_link_port_mutex_create();
    h->ack_sem = host_link_port_sem_create();
    h->notify_sem = host_link_port_sem_create();
    if (h->lock == NULL || h->ack_sem == NULL || h->notify_sem == NULL) {
        if (h->notify_sem != NULL) {
            host_link_port_sem_destroy(h->notify_sem);
        }
        if (h->ack_sem != NULL) {
            host_link_port_sem_destroy(h->ack_sem);
        }
        if (h->lock != NULL) {
            host_link_port_mutex_destroy(h->lock);
        }
        host_link_port_free(h);
        return NULL;
    }
    h->ready = 1u;
    h->parse_state = PS_SYNC0;
    return h;
}

void host_link_deinit(struct host_link_handler *h)
{
    if (h == NULL) {
        return;
    }
    h->ready = 0;
    host_link_port_sem_post(h->ack_sem);
    host_link_port_sem_post(h->notify_sem);
    host_link_port_mutex_lock(h->lock);
    for (uint16_t i = 0; i < HOST_LINK_CFG_NOTIFY_DEPTH; i++) {
        free_frame_payload(&h->notify_slot[i]);
    }
    for (uint16_t i = 0; i < HOST_LINK_CFG_ACK_SLOTS; i++) {
        free_frame_payload(&h->ack_slot[i]);
    }
    for (uint16_t i = 0; i < HOST_LINK_CFG_ASYNC_MAX; i++) {
        h->async_slot[i].used = 0;
    }
    host_link_port_mutex_unlock(h->lock);
    if (h->pend_payload != NULL) {
        host_link_port_free(h->pend_payload);
        h->pend_payload = NULL;
    }
    host_link_port_sem_destroy(h->ack_sem);
    host_link_port_sem_destroy(h->notify_sem);
    host_link_port_mutex_destroy(h->lock);
    host_link_port_free(h);
}

void host_link_poll(struct host_link_handler *h)
{
    if (h == NULL) {
        return;
    }
    for (uint16_t i = 0; i < HOST_LINK_CFG_NOTIFY_DEPTH; i++) {
        if (!h->ready) {
            return;
        }
        host_link_frame_t fr;
        uint8_t have = 0;
        host_link_port_mutex_lock(h->lock);
        if (h->notify_slot[i].is_valid) {
            memcpy(&fr, &h->notify_slot[i], sizeof(fr));
            h->notify_slot[i].payload = NULL;
            h->notify_slot[i].is_valid = 0;
            have = 1;
        }
        host_link_port_mutex_unlock(h->lock);
        if (have) {
            h->notify_cb(h->user_ctx, h, &fr);
            free_frame_payload(&fr);
        }
    }

    uint32_t now = host_link_port_tick_ms();
    host_link_port_mutex_lock(h->lock);
    for (uint16_t i = 0; i < HOST_LINK_CFG_ACK_SLOTS; i++) {
        if (h->ack_slot[i].is_valid) {
            if (host_link_port_tick_diff_ms(h->ack_tick[i], now) >= HOST_LINK_CFG_ACK_TIMEOUT_MS) {
                free_frame_payload(&h->ack_slot[i]);
            }
        }
    }
    for (uint16_t i = 0; i < HOST_LINK_CFG_ASYNC_MAX; i++) {
        if (h->async_slot[i].used
            && host_link_port_tick_diff_ms(h->async_slot[i].started_ms, now) >= HOST_LINK_CFG_ACK_TIMEOUT_MS) {
            host_link_request_done_fn cb = h->async_slot[i].cb;
            void *u = h->async_slot[i].cb_user;
            h->async_slot[i].used = 0;
            host_link_port_mutex_unlock(h->lock);
            if (cb != NULL) {
                cb(u, HOST_LINK_ERR_TIMEOUT, NULL, 0);
            }
            host_link_port_mutex_lock(h->lock);
        }
    }
    host_link_port_mutex_unlock(h->lock);

    if (h->ready) {
        host_link_port_sem_wait(h->notify_sem, HOST_LINK_CFG_POLL_WAIT_MS);
    } else {
        host_link_port_delay_ms(HOST_LINK_CFG_POLL_WAIT_MS);
    }
}

static uint16_t alloc_frame_id(struct host_link_handler *h)
{
    host_link_port_mutex_lock(h->lock);
    uint16_t id = h->next_frame_id++;
    host_link_port_mutex_unlock(h->lock);
    return id;
}

int host_link_request(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len, void **resp_data,
                      uint16_t *resp_len)
{
    if (h == NULL || (req_len > 0u && req_data == NULL)) {
        return HOST_LINK_ERR_INVALID_ARG;
    }
    if (req_len > HOST_LINK_CFG_MAX_PAYLOAD) {
        return HOST_LINK_ERR_INVALID_SIZE;
    }

    unsigned retry = 0;
    int ret = HOST_LINK_ERR_TIMEOUT;
    while (retry < HOST_LINK_CFG_REQUEST_RETRY && ret != HOST_LINK_OK) {
        uint16_t fid = alloc_frame_id(h);
        host_link_header_t hdr = {0};
        hdr.magic = HOST_LINK_MAGIC;
        hdr.version = HOST_LINK_VERSION_CURRENT;
        hdr.reserved = 0;
        hdr.frame_id = fid;
        hdr.type = HOST_LINK_TYPE_REQUEST;
        hdr.reserved2 = 0;
        hdr.cmd = cmd;
        hdr.len = req_len;

        ret = send_wire(h, &hdr, (const uint8_t *)req_data, req_len);
        if (ret != HOST_LINK_OK) {
            retry++;
            continue;
        }

        host_link_frame_t ack = {0};
        ret = wait_for_ack(h, HOST_LINK_TYPE_RESPONSE, cmd, fid, &ack);
        if (ret == HOST_LINK_OK && ack.header.len > 0u && ack.payload != NULL && resp_data != NULL && resp_len != NULL) {
            *resp_data = ack.payload;
            *resp_len = ack.header.len;
            ack.payload = NULL;
        } else if (ack.payload != NULL) {
            host_link_port_free(ack.payload);
        }
        if (ret != HOST_LINK_OK) {
            retry++;
        }
    }
    return ret;
}

int host_link_request_async(host_link_handler_t *h, uint16_t cmd, const void *req_data, uint16_t req_len,
                            host_link_request_done_fn done, void *done_user)
{
    if (h == NULL || done == NULL || (req_len > 0u && req_data == NULL)) {
        return HOST_LINK_ERR_INVALID_ARG;
    }
    if (req_len > HOST_LINK_CFG_MAX_PAYLOAD) {
        return HOST_LINK_ERR_INVALID_SIZE;
    }

    host_link_port_mutex_lock(h->lock);
    int slot = -1;
    for (uint16_t i = 0; i < HOST_LINK_CFG_ASYNC_MAX; i++) {
        if (!h->async_slot[i].used) {
            slot = (int)i;
            break;
        }
    }
    if (slot < 0) {
        host_link_port_mutex_unlock(h->lock);
        return HOST_LINK_ERR_NO_SLOT;
    }
    uint16_t fid = h->next_frame_id++;
    h->async_slot[slot].used = 1;
    h->async_slot[slot].id = fid;
    h->async_slot[slot].cmd = cmd;
    h->async_slot[slot].started_ms = host_link_port_tick_ms();
    h->async_slot[slot].cb = done;
    h->async_slot[slot].cb_user = done_user;
    host_link_port_mutex_unlock(h->lock);

    host_link_header_t hdr = {0};
    hdr.magic = HOST_LINK_MAGIC;
    hdr.version = HOST_LINK_VERSION_CURRENT;
    hdr.reserved = 0;
    hdr.frame_id = fid;
    hdr.type = HOST_LINK_TYPE_REQUEST;
    hdr.reserved2 = 0;
    hdr.cmd = cmd;
    hdr.len = req_len;

    int ret = send_wire(h, &hdr, (const uint8_t *)req_data, req_len);
    if (ret != HOST_LINK_OK) {
        host_link_port_mutex_lock(h->lock);
        h->async_slot[slot].used = 0;
        host_link_port_mutex_unlock(h->lock);
        done(done_user, ret, NULL, 0);
    }
    return ret;
}

int host_link_response(host_link_handler_t *h, const host_link_frame_t *req_frame, const void *data, uint16_t len)
{
    if (h == NULL || req_frame == NULL || (len > 0u && data == NULL)) {
        return HOST_LINK_ERR_INVALID_ARG;
    }
    if (len > HOST_LINK_CFG_MAX_PAYLOAD) {
        return HOST_LINK_ERR_INVALID_SIZE;
    }
    host_link_header_t hdr = {0};
    hdr.magic = HOST_LINK_MAGIC;
    hdr.version = HOST_LINK_VERSION_CURRENT;
    hdr.reserved = 0;
    hdr.frame_id = req_frame->header.frame_id;
    hdr.type = HOST_LINK_TYPE_RESPONSE;
    hdr.reserved2 = 0;
    hdr.cmd = req_frame->header.cmd;
    hdr.len = len;
    return send_wire(h, &hdr, (const uint8_t *)data, len);
}

int host_link_send_event(host_link_handler_t *h, uint16_t cmd, const void *data, uint16_t len)
{
    if (h == NULL || (len > 0u && data == NULL)) {
        return HOST_LINK_ERR_INVALID_ARG;
    }
    if (len > HOST_LINK_CFG_MAX_PAYLOAD) {
        return HOST_LINK_ERR_INVALID_SIZE;
    }
    unsigned retry = 0;
    int ret = HOST_LINK_ERR_TIMEOUT;
    while (retry < HOST_LINK_CFG_REQUEST_RETRY && ret != HOST_LINK_OK) {
        uint16_t fid = alloc_frame_id(h);
        host_link_header_t hdr = {0};
        hdr.magic = HOST_LINK_MAGIC;
        hdr.version = HOST_LINK_VERSION_CURRENT;
        hdr.reserved = 0;
        hdr.frame_id = fid;
        hdr.type = HOST_LINK_TYPE_EVENT;
        hdr.reserved2 = 0;
        hdr.cmd = cmd;
        hdr.len = len;

        ret = send_wire(h, &hdr, (const uint8_t *)data, len);
        if (ret != HOST_LINK_OK) {
            retry++;
            continue;
        }
        host_link_frame_t ack = {0};
        ret = wait_for_ack(h, HOST_LINK_TYPE_EVENT_ACK, cmd, fid, &ack);
        if (ack.payload != NULL) {
            host_link_port_free(ack.payload);
        }
        if (ret != HOST_LINK_OK) {
            retry++;
        }
    }
    return ret;
}

int host_link_event_ack(host_link_handler_t *h, const host_link_frame_t *event_frame)
{
    if (h == NULL || event_frame == NULL) {
        return HOST_LINK_ERR_INVALID_ARG;
    }
    host_link_header_t hdr = {0};
    hdr.magic = HOST_LINK_MAGIC;
    hdr.version = HOST_LINK_VERSION_CURRENT;
    hdr.reserved = 0;
    hdr.frame_id = event_frame->header.frame_id;
    hdr.type = HOST_LINK_TYPE_EVENT_ACK;
    hdr.reserved2 = 0;
    hdr.cmd = event_frame->header.cmd;
    hdr.len = 0;
    return send_wire(h, &hdr, NULL, 0);
}
