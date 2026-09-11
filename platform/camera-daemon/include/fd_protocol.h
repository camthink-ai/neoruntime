/**
 * @file fd_protocol.h
 * @brief FD Publisher Wire Protocol - DMA-BUF FD passing over Unix Domain Sockets
 *
 * Binary protocol between camera-daemon and App containers for zero-copy
 * frame delivery via SCM_RIGHTS.
 *
 * Flow:
 *   Client → Server: SUBSCRIBE (stream_name)
 *   Server → Client: OK / ERROR
 *   Server → Client: FRAME (metadata + SCM_RIGHTS fds)  [repeated]
 *   Client → Server: RELEASE (frame_id)                  [per frame]
 *   Client → Server: UNSUBSCRIBE
 */

#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FD_PUB_MAX_STREAM_NAME  64
#define FD_PUB_MAX_FDS          3       /* Max DMA-BUF fds per frame (planes) */
#define FD_PUB_DSP_MAX_FDS      64      /* Max fds in a DSP alloc response */
#define FD_PUB_PROTOCOL_VERSION 1

/* ========== Message types ========== */
typedef enum {
    FD_PUB_MSG_SUBSCRIBE    = 1,
    FD_PUB_MSG_UNSUBSCRIBE  = 2,
    FD_PUB_MSG_FRAME        = 3,
    FD_PUB_MSG_RELEASE      = 4,
    FD_PUB_MSG_OK           = 5,
    FD_PUB_MSG_ERROR        = 6,
    /* DSP offload buffer plane (PLAT-5): buffers are allocated/freed over
     * this socket (fds via SCM_RIGHTS) and referenced by id in the
     * SubmitDspJob gRPC. */
    FD_PUB_MSG_DSP_ALLOC        = 7,    /* client → server */
    FD_PUB_MSG_DSP_ALLOC_RESP   = 8,    /* server → client, fds attached */
    FD_PUB_MSG_DSP_BUF_RELEASE  = 9,    /* client → server */
    /* Zero-copy source import: the client already holds dma-buf fds (e.g. a
     * subscribed frame kept alive via FD_PUB_MSG_RELEASE deferral) and hands
     * them to the daemon instead of copying pixels into a DSP pool buffer.
     * An import occupies the same id namespace as pool buffers, so the id is
     * usable as SubmitDspJob src_buffer_id and freed with DSP_BUF_RELEASE. */
    FD_PUB_MSG_DSP_IMPORT       = 10,   /* client → server, fds attached */
    FD_PUB_MSG_DSP_IMPORT_RESP  = 11,   /* server → client */
} FdPubMsgType;

/* ========== Message header (all messages start with this) ========== */
typedef struct {
    uint32_t type;          /* FdPubMsgType */
    uint32_t size;          /* Total message size including header */
} FdPubMsgHeader;

/* ========== Client → Server: Subscribe request ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_SUBSCRIBE */
    uint32_t version;       /* Protocol version */
    char stream_name[FD_PUB_MAX_STREAM_NAME];
} FdPubSubscribeMsg;

/* ========== Server → Client: Frame delivery (sent with SCM_RIGHTS) ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_FRAME */
    uint64_t frame_id;      /* Unique frame ID (must be sent back in RELEASE) */
    uint64_t timestamp_ns;  /* Capture timestamp */
    uint64_t sequence;      /* Frame sequence number */
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* HalPixelFormat */
    uint32_t num_planes;
    uint32_t strides[3];    /* Stride per plane */
    uint32_t sizes[3];      /* Size per plane in bytes */
    uint32_t num_fds;       /* Number of DMA-BUF fds attached */
} FdPubFrameMsg;

/* ========== Client → Server: Release frame ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_RELEASE */
    uint64_t frame_id;      /* Frame ID from FdPubFrameMsg */
} FdPubReleaseMsg;

/* ========== Server → Client: Response ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_OK or FD_PUB_MSG_ERROR */
    int32_t code;           /* 0 = success, < 0 = error code */
} FdPubResponseMsg;

/* ========== Client → Server: DSP buffer allocation request (PLAT-5) ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_DSP_ALLOC */
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* HalPixelFormat (NV12=0, RGB24=4, GRAY8=8) */
    uint32_t count;         /* buffers to allocate; count*num_planes <= 64 */
} FdPubDspAllocMsg;

/* ========== Server → Client: DSP buffer allocation response ==============
 * The dma-buf fds are attached via SCM_RIGHTS, buffer-major order
 * (num_planes fds per buffer, count*num_planes total). Ids are daemon-side
 * handles passed by value in SubmitDspJob. Strides/sizes are identical for
 * every buffer in one allocation (same geometry request).
 * On failure code < 0 and no fds are attached. */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_DSP_ALLOC_RESP */
    int32_t code;           /* 0 = success, < 0 = error code */
    uint32_t count;         /* number of buffers actually allocated */
    uint32_t num_planes;
    uint32_t strides[3];
    uint32_t sizes[3];
    uint64_t buffer_ids[FD_PUB_DSP_MAX_FDS];
} FdPubDspAllocRespMsg;

/* ========== Client → Server: DSP buffer release ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_DSP_BUF_RELEASE */
    uint64_t buffer_id;     /* id from FdPubDspAllocRespMsg or IMPORT_RESP */
} FdPubDspBufReleaseMsg;

/* ========== Client → Server: zero-copy source import ==============
 * The num_planes dma-buf fds are attached via SCM_RIGHTS. The daemon does
 * NOT take ownership of the client's fds: it dups them, so the client may
 * close its copies whenever it likes (the import stays valid on its own
 * dup). Geometry must describe the actual buffer layout; the daemon only
 * sanity-checks ranges, not the underlying allocation. */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_DSP_IMPORT */
    uint32_t width;
    uint32_t height;
    uint32_t format;        /* HalPixelFormat (NV12=0, RGB24=4, GRAY8=8) */
    uint32_t num_planes;    /* fds attached; 1..3 */
    uint32_t strides[3];
    uint32_t sizes[3];
} FdPubDspImportMsg;

/* ========== Server → Client: import response ========== */
typedef struct {
    FdPubMsgHeader hdr;     /* type = FD_PUB_MSG_DSP_IMPORT_RESP */
    int32_t code;           /* 0 = success, < 0 = error code */
    uint64_t import_id;     /* valid iff code == 0 */
} FdPubDspImportRespMsg;

/* ========== Helper: Send message with optional FDs via SCM_RIGHTS ==========
 * General form: the ancillary buffer is sized for `fd_capacity` fds (must
 * be >= num_fds). Callers passing more than a few fds (e.g. the DSP alloc
 * response, up to FD_PUB_DSP_MAX_FDS) must heap-allocate via this path.
 *
 * Returns 0 when the full message was sent, -1 otherwise.
 * On a partial send errno is set to EMSGSIZE: with SCM_RIGHTS the fds cross
 * with the FIRST byte queued, so a partial send has already leaked the fds
 * to the receiver and desynced its stream — callers must treat that as a
 * hard failure, not retry. sendmsg() returns EAGAIN only when nothing at
 * all was queued, so an EAGAIN failure never transfers fds. */
static inline int fd_pub_sendmsg_capped_flags(int sock_fd, const void* data, size_t data_len,
                                              const int* fds, int num_fds, int fd_capacity,
                                              int extra_flags) {
    struct msghdr msg;
    struct iovec iov;
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = (void*)data;
    iov.iov_len = data_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf_stack[CMSG_SPACE(sizeof(int) * FD_PUB_MAX_FDS)];
    char* cmsg_buf = cmsg_buf_stack;
    if (fd_capacity > FD_PUB_MAX_FDS) {
        cmsg_buf = (char*)calloc(1, CMSG_SPACE(sizeof(int) * (size_t)fd_capacity));
        if (!cmsg_buf) return -1;
    }

    if (fds && num_fds > 0) {
        memset(cmsg_buf, 0, CMSG_SPACE(sizeof(int) * (size_t)fd_capacity));
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * (size_t)num_fds);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * (size_t)num_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * (size_t)num_fds);
    }

    ssize_t sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL | extra_flags);
    if (cmsg_buf != cmsg_buf_stack) free(cmsg_buf);
    if (sent == (ssize_t)data_len) return 0;
    if (sent >= 0) errno = EMSGSIZE;    /* partial send: fds already crossed */
    return -1;
}

/* Blocking form (control-plane replies: backpressure is acceptable there). */
static inline int fd_pub_sendmsg_capped(int sock_fd, const void* data, size_t data_len,
                                        const int* fds, int num_fds, int fd_capacity) {
    return fd_pub_sendmsg_capped_flags(sock_fd, data, data_len, fds, num_fds,
                                       fd_capacity, 0);
}

/* Original 3-fd form (frames). */
static inline int fd_pub_sendmsg(int sock_fd, const void* data, size_t data_len,
                                  const int* fds, int num_fds) {
    if (num_fds > FD_PUB_MAX_FDS) return -1;
    return fd_pub_sendmsg_capped(sock_fd, data, data_len, fds, num_fds,
                                 FD_PUB_MAX_FDS);
}

/* Non-blocking 3-fd form: frame delivery runs on the dispatch thread and
 * must never stall on a slow client. EAGAIN maps to "client too slow". */
static inline int fd_pub_sendmsg_flags(int sock_fd, const void* data, size_t data_len,
                                       const int* fds, int num_fds, int flags) {
    if (num_fds > FD_PUB_MAX_FDS) return -1;
    return fd_pub_sendmsg_capped_flags(sock_fd, data, data_len, fds, num_fds,
                                       FD_PUB_MAX_FDS, flags);
}

/* ========== Helper: Receive message with optional FDs ========== */
static inline int fd_pub_recvmsg(int sock_fd, void* data, size_t data_len,
                                  int* fds, int* out_num_fds, int max_fds) {
    struct msghdr msg;
    struct iovec iov;
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = data;
    iov.iov_len = data_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    char cmsg_buf[CMSG_SPACE(sizeof(int) * FD_PUB_MAX_FDS)];
    memset(cmsg_buf, 0, sizeof(cmsg_buf));
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);

    if (out_num_fds) *out_num_fds = 0;

    ssize_t received = recvmsg(sock_fd, &msg, 0);
    if (received <= 0) return -1;

    /* Extract FDs from ancillary data */
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
    if (cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS) {
        int nfds = (cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        if (nfds > max_fds) nfds = max_fds;
        if (fds) memcpy(fds, CMSG_DATA(cmsg), sizeof(int) * nfds);
        if (out_num_fds) *out_num_fds = nfds;
    }

    return (int)received;
}

#ifdef __cplusplus
}
#endif
