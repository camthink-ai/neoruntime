/**
 * @file fd_publisher.h
 * @brief FD Publisher - Zero-copy DMA-BUF FD delivery to App containers
 *
 * Listens on a Unix Domain Socket. Apps connect, subscribe to a stream,
 * and receive DMA-BUF file descriptors via SCM_RIGHTS.
 *
 * For trusted Apps that declare `dma_buf: true` in their manifest.
 *
 * Thread model:
 *   - 1 accept thread (listens on UDS)
 *   - 1 recv thread per client (handles SUBSCRIBE/RELEASE)
 *   - Frame delivery happens on FrameRouter callback thread (non-blocking send)
 */

#pragma once

#include <string>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <thread>
#include <atomic>
#include <cstdint>

struct ManagedFrame;
class FrameRouter;

struct FdPublisherConfig {
    std::string sock_path = "/run/aipc/camera.sock";
    uint32_t    max_clients = 16;
    uint32_t    max_outstanding_per_client = 3;
};

class FdPublisher {
public:
    explicit FdPublisher(FrameRouter* router, const FdPublisherConfig& config);
    ~FdPublisher();

    FdPublisher(const FdPublisher&) = delete;
    FdPublisher& operator=(const FdPublisher&) = delete;

    /** Start UDS server and accept thread */
    bool start();

    /** Stop server and disconnect all clients */
    void stop();

    /**
     * @brief Deliver frame to all FD clients subscribed to this stream.
     *
     * Called from FrameRouter callback thread. For each client:
     *   - If outstanding >= max_outstanding → skip (frame dropped for this client)
     *   - retain(mf) to bump ref count
     *   - sendmsg(SCM_RIGHTS, dma_fds) with frame metadata
     *   - Track in client's outstanding map
     *
     * After iterating all clients, releases the original ref.
     */
    void on_frame(const std::string& stream_name, ManagedFrame* mf);

    /** Number of connected FD clients */
    uint32_t client_count() const;

    /** Number of FD clients subscribed to a specific stream */
    uint32_t stream_client_count(const std::string& stream_name) const;

    struct Stats {
        uint64_t frames_sent = 0;
        uint64_t frames_dropped = 0;     // Client too slow
        uint64_t send_errors = 0;
        uint64_t clients_connected = 0;
        uint64_t clients_disconnected = 0;
    };
    Stats get_stats() const;

private:
    /* ---- Per-client state ---- */
    struct ClientState {
        int         fd = -1;
        std::string stream_name;
        bool        subscribed = false;
        std::thread recv_thread;

        // Outstanding frames: frame_id → ManagedFrame*
        std::mutex  outstanding_mu;
        std::unordered_map<uint64_t, ManagedFrame*> outstanding;
    };

    FrameRouter*        router_;
    FdPublisherConfig   config_;

    // Server
    int                 server_fd_ = -1;
    std::thread         accept_thread_;
    std::atomic<bool>   running_{false};

    // Client registry
    mutable std::mutex  clients_mu_;
    std::unordered_map<int, ClientState*> clients_;   // client_fd → state

    // Stats
    mutable std::mutex  stats_mu_;
    Stats               stats_;

    /* ---- Internal methods ---- */
    void accept_loop();
    void client_recv_loop(ClientState* client);
    void handle_subscribe(ClientState* client, const void* msg_data);
    void handle_release(ClientState* client, const void* msg_data);
    void disconnect_client(int client_fd);
    void release_all_outstanding(ClientState* client);

    /** Send FdPubFrameMsg + SCM_RIGHTS to one client. Returns true on success. */
    bool send_frame_to_client(ClientState* client, ManagedFrame* mf);
};
