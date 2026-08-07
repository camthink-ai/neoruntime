/**
 * @file rtsp_server.h
 * @brief Lightweight embedded RTSP server for camera-daemon
 *
 * Supports:
 * - H264/H265 over RTP (TCP interleaved transport)
 * - Multiple concurrent clients
 * - Multiple streams (e.g., rtsp://device:8554/main, /sub)
 * - Automatic SPS/PPS/VPS extraction from bitstream
 */

#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <cstdint>
#include <memory>

extern "C" {
    #include "hal_buffer.h"
}

class RtspServer {
public:
    struct StreamInfo {
        std::string name;       // URL path segment: /main, /sub
        std::string codec;      // "h264", "h265", or "pcm"
        uint32_t width  = 0;
        uint32_t height = 0;
        uint32_t fps    = 0;

        // Audio-specific fields
        bool     is_audio    = false;
        uint32_t sample_rate = 48000;
        uint32_t channels    = 1;
    };

    RtspServer();
    ~RtspServer();

    RtspServer(const RtspServer&) = delete;
    RtspServer& operator=(const RtspServer&) = delete;

    /** Register a stream (call before start) */
    void add_stream(const StreamInfo& info);

    /** Start RTSP server on given port */
    bool start(uint16_t port = 8554);

    /** Stop server and disconnect all clients */
    void stop();

    /** Feed encoded packet (thread-safe, called from encoder callback) */
    void on_packet(const std::string& stream_name, const HalPacketBuffer* packet);

    /** Callback to request keyframe (set by CameraDaemon) */
    using KeyframeRequestFn = std::function<void(const std::string& stream_name)>;
    void set_keyframe_request_cb(KeyframeRequestFn fn);

    /** Associate audio with video streams for multi-track SDP.
     *  After calling this, DESCRIBE on video streams will include
     *  both m=video and m=audio sections. */
    void set_audio_info(const std::string& audio_stream_name);

private:
    /* ---- NAL parsing ---- */
    struct NalSpan {
        const uint8_t* data;
        size_t size;
    };
    static std::vector<NalSpan> split_nals(const uint8_t* data, size_t size);
    static uint8_t h264_nal_type(const uint8_t* nal) { return nal[0] & 0x1F; }
    static uint8_t h265_nal_type(const uint8_t* nal) { return (nal[0] >> 1) & 0x3F; }

    /* ---- Media stream ---- */
    struct MediaStream {
        StreamInfo info;
        std::vector<uint8_t> sps, pps, vps;
        bool params_ready = false;
        std::mutex mu;
    };

    /* ---- Per-client state ---- */
    struct Client {
        int fd = -1;
        int id = 0;

        std::string stream_name;
        std::string session_id;

        enum State { INIT, READY, PLAYING } state = INIT;

        // Video track
        uint8_t rtp_channel  = 0;
        uint8_t rtcp_channel = 1;
        uint16_t rtp_seq     = 0;
        uint32_t rtp_ssrc    = 0;
        uint64_t rtp_ts_base = 0;
        bool     ts_base_set = false;
        uint32_t rtp_frame_count = 0;
        uint32_t pkt_count   = 0;
        uint32_t octet_count = 0;

        // Audio track (multi-track: video stream with embedded audio)
        bool     has_audio = false;
        uint8_t audio_rtp_channel  = 2;
        uint8_t audio_rtcp_channel = 3;
        uint16_t audio_rtp_seq     = 0;
        uint32_t audio_rtp_ssrc    = 0;
        uint32_t rtp_audio_sample_count = 0;

        std::mutex write_mu;
        std::string read_buf;
        bool alive = true;
    };

    /* ---- RTSP parsing ---- */
    struct RtspRequest {
        std::string method;
        std::string uri;
        std::unordered_map<std::string, std::string> headers;
        int cseq = 0;
    };

    bool parse_request(const std::string& raw, RtspRequest& req);
    std::string extract_stream_name(const std::string& uri);

    void handle_request(Client& c, const RtspRequest& req);
    void reply_options(Client& c, const RtspRequest& req);
    void reply_describe(Client& c, const RtspRequest& req);
    void reply_setup(Client& c, const RtspRequest& req);
    void reply_play(Client& c, const RtspRequest& req);
    void reply_teardown(Client& c, const RtspRequest& req);
    void reply_get_parameter(Client& c, const RtspRequest& req);
    void send_response(Client& c, int code, const std::string& reason,
                       int cseq,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& body = "");

    std::string build_sdp(const MediaStream& ms);

    static constexpr size_t RTP_MTU    = 1400;
    static constexpr uint8_t RTP_PT    = 96;
    static constexpr uint8_t RTP_AUDIO_PT = 97;  // Dynamic payload type for L16 audio
    static constexpr uint32_t RTP_CLOCK = 90000;

    void send_rtp_h264(Client& c, const uint8_t* nal, size_t len,
                       uint32_t ts, bool marker);
    void send_rtp_h265(Client& c, const uint8_t* nal, size_t len,
                       uint32_t ts, bool marker);
    void send_rtp_raw(Client& c, const uint8_t* payload, size_t len,
                      bool marker, uint32_t ts);
    void send_tcp_interleaved(Client& c, uint8_t channel,
                              const uint8_t* data, size_t len);

    void server_loop();
    void handle_client_data(int client_id);
    void remove_client(int client_id);

    static std::string base64_encode(const uint8_t* data, size_t len);
    static std::string generate_session_id();
    static uint32_t generate_ssrc();

    std::atomic<bool> running_{false};
    uint16_t port_ = 8554;
    int listen_fd_ = -1;
    int epoll_fd_  = -1;
    std::thread server_thread_;

    std::unordered_map<std::string, MediaStream> streams_;

    std::mutex clients_mu_;
    std::unordered_map<int, std::unique_ptr<Client>> clients_;
    int next_client_id_ = 1;

    KeyframeRequestFn keyframe_request_fn_;

    // Audio association for multi-track
    std::string audio_stream_name_;   // e.g. "audio_capture"
    bool has_audio_stream_ = false;
};
