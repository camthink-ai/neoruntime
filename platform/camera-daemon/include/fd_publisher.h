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
class DspService;

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
     * Called from FrameRouter dispatch thread. The whole pass runs under
     * clients_mu_ (sends are non-blocking, so bounded): a disconnecting
     * client cannot be freed or closed mid-iteration, and its
     * release_all_outstanding() cannot interleave with our tracking.
     * For each client:
     *   - If outstanding >= max_outstanding → skip (frame dropped for this client)
     *   - retain(mf), then track in outstanding BEFORE sendmsg, so a RELEASE
     *     arriving right after delivery always finds its entry (the old
     *     send-then-track order discarded such RELEASEs and pinned the slot)
     *   - sendmsg(SCM_RIGHTS, dma_fds) with MSG_DONTWAIT; EAGAIN drops the
     *     frame for that client, a hard/partial send drops the client
     *
     * After iterating all clients, releases the original ref.
     */
    void on_frame(const std::string& stream_name, ManagedFrame* mf);

    /** Number of connected FD clients */
    uint32_t client_count() const;

    /** Number of FD clients subscribed to a specific stream */
    uint32_t stream_client_count(const std::string& stream_name) const;

    /**
     * @brief Wire the DSP offload buffer plane into this socket (PLAT-5).
     *
     * Must be called before start(). Enables DSP_ALLOC / DSP_BUF_RELEASE
     * handling on client recv threads and buffer cleanup on disconnect.
     * The service must outlive this publisher (or be stopped first).
     */
    void set_dsp_service(DspService* dsp_service);

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
    DspService*         dsp_service_ = nullptr;  // optional; set before start()

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
    /* PLAT-5 DSP buffer plane: DSP_ALLOC and DSP_BUF_RELEASE handlers.
     * Called on the client's recv thread; alloc replies carry fds. */
    void handle_dsp_alloc(ClientState* client, const void* msg_data);
    void handle_dsp_buf_release(ClientState* client, const void* msg_data);
    /* Zero-copy source import. fds arrived via SCM_RIGHTS; the handler
     * closes every received fd (import_buffer dups what it keeps). */
    void handle_dsp_import(ClientState* client, const void* msg_data,
                           const int* fds, int num_fds);
    void disconnect_client(int client_fd);
    void release_all_outstanding(ClientState* client);
    /** Remove one outstanding entry (used to undo a tracked-but-unsent frame). */
    void erase_outstanding(ClientState* client, uint64_t frame_id);

    /** Frame send outcome, consumed by the dispatch loop. */
    enum class FrameSendResult {
        kOk,            /**< Full message queued */
        kSlowClient,    /**< EAGAIN — nothing queued, no fds crossed; drop frame, keep client */
        kHardError,     /**< Error or partial send — stream desynced or fd broken; drop client */
        kUndeliverable, /**< Frame not fd-passable (no dma-buf fds); drop frame, keep client */
    };

    /** Send FdPubFrameMsg + SCM_RIGHTS to one client (non-blocking). */
    FrameSendResult send_frame_to_client(ClientState* client, ManagedFrame* mf);
};
