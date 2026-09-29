#include "buffer_lookup.h"
#include "log.h"
#include "fd_protocol.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstring>

namespace aipc::ai_runtime {

namespace {

// One round-trip must not hang an Infer thread forever if the daemon wedges.
const timeval kIoTimeout{2, 0};

// Short text for the daemon's DspService error codes (dsp_service.h); the wire
// response carries only the numeric code.
const char* dsp_svc_error_text(int code) {
    switch (code) {
        case -1: return "invalid buffer descriptor";
        case -2: return "unknown or foreign buffer id";
        case -3: return "quota exceeded";
        case -4: return "timeout";
        case -5: return "dsp service unavailable";
        case -6: return "out of memory";
        case -7: return "per-client buffer limit";
        default:  return "daemon error";
    }
}

// Drain the remainder of a struct that arrived fragmented (SOCK_STREAM has no
// message boundaries). The SCM_RIGHTS fds, if any, ride with the first byte
// and were already extracted by fd_pub_recvmsg — what is left is plain bytes.
bool recv_rest(int fd, void* data, size_t have, size_t want) {
    char* p = static_cast<char*>(data) + have;
    size_t left = want - have;
    while (left > 0) {
        ssize_t n = ::recv(fd, p, left, MSG_WAITALL);
        if (n <= 0) return false;
        p += n;
        left -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

BufferLookupClient::BufferLookupClient(const std::string& socket_path)
    : socket_path_(socket_path) {}

BufferLookupClient::~BufferLookupClient() {
    if (sock_fd_ >= 0) {
        // Any leases still held on this connection are reaped by the daemon's
        // disconnect cleanup (lookup_release_all), so a bare close is safe.
        ::close(sock_fd_);
    }
}

int BufferLookupClient::ensure_connected_locked() {
    if (sock_fd_ >= 0) return sock_fd_;

    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        LOG_ERROR("BufferLookup: socket() failed: %s", strerror(errno));
        return -1;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("BufferLookup: connect(%s) failed: %s",
                  socket_path_.c_str(), strerror(errno));
        ::close(fd);
        return -1;
    }

    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &kIoTimeout, sizeof(kIoTimeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &kIoTimeout, sizeof(kIoTimeout));

    sock_fd_ = fd;
    LOG_INFO("BufferLookup: connected to %s", socket_path_.c_str());
    return sock_fd_;
}

BufferLookupClient::LookupResult BufferLookupClient::lookup(uint64_t buffer_id) {
    std::lock_guard lock(mu_);
    LookupResult out;

    int fd = ensure_connected_locked();
    if (fd < 0) {
        out.rc = -ENODEV;
        out.message = "cannot reach camera-daemon buffer registry";
        return out;
    }

    FdPubDspLookupMsg msg{};
    msg.hdr.type = FD_PUB_MSG_DSP_LOOKUP;
    msg.hdr.size = sizeof(msg);
    msg.buffer_id = buffer_id;

    if (fd_pub_sendmsg(fd, &msg, sizeof(msg), nullptr, 0) != 0) {
        LOG_ERROR("BufferLookup: LOOKUP send failed (id=%lu): %s",
                  (unsigned long)buffer_id, strerror(errno));
        ::close(fd);
        sock_fd_ = -1;
        out.rc = -ENODEV;
        out.message = "camera-daemon connection write failed";
        return out;
    }

    FdPubDspLookupRespMsg resp{};
    int fds[FD_PUB_MAX_FDS];
    int num_fds = 0;
    int n = fd_pub_recvmsg(fd, &resp, sizeof(resp), fds, &num_fds, FD_PUB_MAX_FDS);
    if (n <= 0) {
        LOG_ERROR("BufferLookup: LOOKUP recv failed (id=%lu): %s",
                  (unsigned long)buffer_id, strerror(errno));
        ::close(fd);
        sock_fd_ = -1;
        out.rc = -ENODEV;
        out.message = "camera-daemon connection read failed";
        return out;
    }
    if (static_cast<size_t>(n) < sizeof(resp) &&
        !recv_rest(fd, &resp, static_cast<size_t>(n), sizeof(resp))) {
        LOG_ERROR("BufferLookup: LOOKUP resp truncated (id=%lu)", (unsigned long)buffer_id);
        ::close(fd);
        sock_fd_ = -1;
        out.rc = -EPROTO;
        out.message = "truncated lookup response";
        return out;
    }

    if (resp.hdr.type != FD_PUB_MSG_DSP_LOOKUP_RESP) {
        // This connection only ever carries LOOKUP traffic, so anything else
        // means stream desync — drop the connection, not just this reply.
        LOG_ERROR("BufferLookup: unexpected reply type %u (id=%lu)",
                  resp.hdr.type, (unsigned long)buffer_id);
        for (int i = 0; i < num_fds; ++i) ::close(fds[i]);
        ::close(fd);
        sock_fd_ = -1;
        out.rc = -EPROTO;
        out.message = "protocol desync on lookup connection";
        return out;
    }

    if (resp.code != 0) {
        out.rc = resp.code;
        out.message = dsp_svc_error_text(resp.code);
        LOG_WARN("BufferLookup: LOOKUP id=%lu rejected: %d (%s)",
                 (unsigned long)buffer_id, resp.code, out.message.c_str());
        return out;
    }

    if (num_fds != static_cast<int>(resp.num_planes) || resp.num_planes == 0 ||
        resp.num_planes > FD_PUB_MAX_FDS) {
        // Refuse to trust geometry we cannot map one-fd-per-plane.
        LOG_ERROR("BufferLookup: id=%lu plane/fd mismatch (planes=%u fds=%d)",
                  (unsigned long)buffer_id, resp.num_planes, num_fds);
        for (int i = 0; i < num_fds; ++i) ::close(fds[i]);
        ::close(fd);
        sock_fd_ = -1;
        out.rc = -EPROTO;
        out.message = "lookup response plane/fd mismatch";
        return out;
    }

    out.width = resp.width;
    out.height = resp.height;
    out.format = resp.format;
    out.num_planes = resp.num_planes;
    for (uint32_t i = 0; i < 3; ++i) {
        out.strides[i] = resp.strides[i];
        out.sizes[i] = resp.sizes[i];
    }
    out.fds.assign(fds, fds + num_fds);
    return out;
}

void BufferLookupClient::release(uint64_t buffer_id) {
    std::lock_guard lock(mu_);

    int fd = ensure_connected_locked();
    if (fd < 0) {
        LOG_WARN("BufferLookup: RELEASE id=%lu skipped (no connection; lease "
                 "is reaped on daemon-side disconnect cleanup)",
                 (unsigned long)buffer_id);
        return;
    }

    FdPubDspLookupReleaseMsg msg{};
    msg.hdr.type = FD_PUB_MSG_DSP_LOOKUP_RELEASE;
    msg.hdr.size = sizeof(msg);
    msg.buffer_id = buffer_id;

    if (fd_pub_sendmsg(fd, &msg, sizeof(msg), nullptr, 0) != 0) {
        LOG_WARN("BufferLookup: RELEASE id=%lu send failed: %s (lease is "
                 "reaped on daemon-side disconnect cleanup)",
                 (unsigned long)buffer_id, strerror(errno));
        ::close(fd);
        sock_fd_ = -1;
    }
}

}  // namespace aipc::ai_runtime
