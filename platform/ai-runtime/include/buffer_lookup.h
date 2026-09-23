#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aipc::ai_runtime {

// Resolves Tensor.buffer_id references against camera-daemon's DSP buffer
// registry over the camera.sock UDS (DSP_LOOKUP / DSP_LOOKUP_RELEASE in
// fd_protocol.h).
//
// One persistent connection, fully serialized: each lookup holds mu_ for one
// UDS round-trip plus one fire-and-forget release. On a local socket that is
// microseconds-to-milliseconds — negligible next to the NPU run — and it
// keeps the protocol trivially in-order. A dead connection is closed and
// re-established lazily on the next call.
class BufferLookupClient {
public:
    // Mirrors DspService::LookupResult (daemon side) without pulling HAL
    // headers into every translation unit that includes this file.
    struct LookupResult {
        int rc = 0;  // 0 = ok; <0 = DspServiceError from the daemon, or -ENODEV
        std::string message;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;       // HalPixelFormat (NV12 = 0)
        uint32_t num_planes = 0;
        uint32_t strides[3] = {0, 0, 0};
        uint32_t sizes[3] = {0, 0, 0};
        std::vector<int> fds;      // num_planes dup'd fds on success; CALLER CLOSES
    };

    explicit BufferLookupClient(const std::string& socket_path);
    ~BufferLookupClient();

    BufferLookupClient(const BufferLookupClient&) = delete;
    BufferLookupClient& operator=(const BufferLookupClient&) = delete;

    // LOOKUP buffer_id: pins the frame in the daemon for THIS connection and
    // returns its plane fds + geometry. On rc != 0 no fds are returned.
    LookupResult lookup(uint64_t buffer_id);

    // LOOKUP_RELEASE: drop the lease taken by a successful lookup(). Errors
    // are logged, not surfaced — a lost lease is reaped on disconnect anyway.
    void release(uint64_t buffer_id);

private:
    int ensure_connected_locked();  // (re)connects; returns fd or -1

    std::string socket_path_;
    std::mutex mu_;
    int sock_fd_ = -1;
};

}  // namespace aipc::ai_runtime
