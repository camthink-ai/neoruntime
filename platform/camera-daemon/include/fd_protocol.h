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
#include <sys/socket.h>
#include <sys/un.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FD_PUB_MAX_STREAM_NAME  64
#define FD_PUB_MAX_FDS          3       /* Max DMA-BUF fds per frame (planes) */
#define FD_PUB_PROTOCOL_VERSION 1

/* ========== Message types ========== */
typedef enum {
    FD_PUB_MSG_SUBSCRIBE    = 1,
    FD_PUB_MSG_UNSUBSCRIBE  = 2,
    FD_PUB_MSG_FRAME        = 3,
    FD_PUB_MSG_RELEASE      = 4,
    FD_PUB_MSG_OK           = 5,
    FD_PUB_MSG_ERROR        = 6,
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

/* ========== Helper: Send message with optional FDs via SCM_RIGHTS ========== */
static inline int fd_pub_sendmsg(int sock_fd, const void* data, size_t data_len,
                                  const int* fds, int num_fds) {
    struct msghdr msg;
    struct iovec iov;
    memset(&msg, 0, sizeof(msg));

    iov.iov_base = (void*)data;
    iov.iov_len = data_len;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    /* Ancillary data for FD passing */
    char cmsg_buf[CMSG_SPACE(sizeof(int) * FD_PUB_MAX_FDS)];

    if (fds && num_fds > 0) {
        memset(cmsg_buf, 0, sizeof(cmsg_buf));
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * num_fds);

        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * num_fds);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * num_fds);
    }

    ssize_t sent = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
    return (sent == (ssize_t)data_len) ? 0 : -1;
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
