/**
 * @file rtsp_server.cpp
 * @brief Lightweight embedded RTSP server implementation
 *
 * Protocol: RTSP 1.0 (RFC 2326) + RTP/AVP/TCP (RFC 7826 interleaved)
 * H264 RTP: RFC 6184 (FU-A fragmentation)
 * H265 RTP: RFC 7798 (FU fragmentation)
 */

#include "../include/rtsp_server.h"

extern "C" {
    #include "hal_log.h"
}

#include <sys/socket.h>
#include <sys/epoll.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <random>
#include <chrono>

/* ================================================================
 * Utilities
 * ================================================================ */

static constexpr char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string RtspServer::base64_encode(const uint8_t* data, size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    for (size_t i = 0; i < len; i += 3) {
        uint32_t n = (uint32_t)data[i] << 16;
        if (i + 1 < len) n |= (uint32_t)data[i + 1] << 8;
        if (i + 2 < len) n |= data[i + 2];
        out += B64[(n >> 18) & 0x3F];
        out += B64[(n >> 12) & 0x3F];
        out += (i + 1 < len) ? B64[(n >> 6) & 0x3F] : '=';
        out += (i + 2 < len) ? B64[n & 0x3F] : '=';
    }
    return out;
}

std::string RtspServer::generate_session_id() {
    static std::mt19937_64 rng(std::random_device{}());
    char buf[24];
    snprintf(buf, sizeof(buf), "%016llX",
             (unsigned long long)rng());
    return buf;
}

uint32_t RtspServer::generate_ssrc() {
    static std::mt19937 rng(std::random_device{}());
    return std::uniform_int_distribution<uint32_t>{}(rng);
}

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

/* ================================================================
 * Annex-B NAL unit splitting
 * ================================================================ */

std::vector<RtspServer::NalSpan>
RtspServer::split_nals(const uint8_t* data, size_t size) {
    std::vector<NalSpan> nals;
    if (size < 4) return nals;

    // Collect byte offsets right after each start code
    std::vector<size_t> offsets;
    for (size_t i = 0; i + 2 < size; ) {
        if (data[i] == 0 && data[i + 1] == 0) {
            if (data[i + 2] == 1) {
                offsets.push_back(i + 3);
                i += 3;
                continue;
            }
            if (i + 3 < size && data[i + 2] == 0 && data[i + 3] == 1) {
                offsets.push_back(i + 4);
                i += 4;
                continue;
            }
        }
        i++;
    }

    for (size_t i = 0; i < offsets.size(); i++) {
        const uint8_t* nal = data + offsets[i];
        size_t nal_size;

        if (i + 1 < offsets.size()) {
            // Find beginning of next start code
            size_t next = offsets[i + 1];
            size_t sc_begin = next;
            if (sc_begin >= 4 && data[sc_begin - 4] == 0 &&
                data[sc_begin - 3] == 0 && data[sc_begin - 2] == 0 &&
                data[sc_begin - 1] == 1) {
                sc_begin -= 4;
            } else {
                sc_begin -= 3;
            }
            nal_size = sc_begin - offsets[i];
        } else {
            nal_size = size - offsets[i];
        }

        if (nal_size > 0) {
            nals.push_back({nal, nal_size});
        }
    }

    return nals;
}

/* ================================================================
 * RTSP request parsing
 * ================================================================ */

static std::string str_trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool RtspServer::parse_request(const std::string& raw, RtspRequest& req) {
    std::istringstream ss(raw);
    std::string line;

    // Request line: METHOD URI RTSP/1.0
    if (!std::getline(ss, line)) return false;
    line = str_trim(line);

    auto sp1 = line.find(' ');
    auto sp2 = line.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return false;

    req.method = line.substr(0, sp1);
    req.uri = line.substr(sp1 + 1, sp2 - sp1 - 1);

    // Headers
    while (std::getline(ss, line)) {
        line = str_trim(line);
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = str_trim(line.substr(0, colon));
        std::string val = str_trim(line.substr(colon + 1));
        req.headers[key] = val;
    }

    auto it = req.headers.find("CSeq");
    if (it != req.headers.end()) {
        req.cseq = std::atoi(it->second.c_str());
    }

    return true;
}

std::string RtspServer::extract_stream_name(const std::string& uri) {
    // URI: rtsp://host:port/streamname or /streamname
    // Also handle rtsp://host:port/streamname/trackID=0

    std::string path;
    auto scheme = uri.find("://");
    if (scheme != std::string::npos) {
        auto slash = uri.find('/', scheme + 3);
        path = (slash != std::string::npos) ? uri.substr(slash) : "/";
    } else {
        path = uri;
    }

    // Remove leading /
    if (!path.empty() && path[0] == '/') path = path.substr(1);

    // Remove trailing /trackID=X
    auto track = path.find("/trackID");
    if (track != std::string::npos) path = path.substr(0, track);
    track = path.find("/track");
    if (track != std::string::npos) path = path.substr(0, track);

    return path;
}

/* ================================================================
 * RTSP response helpers
 * ================================================================ */

void RtspServer::send_response(
        Client& c, int code, const std::string& reason, int cseq,
        const std::vector<std::pair<std::string, std::string>>& headers,
        const std::string& body) {

    std::ostringstream ss;
    ss << "RTSP/1.0 " << code << " " << reason << "\r\n";
    ss << "CSeq: " << cseq << "\r\n";

    for (auto& [k, v] : headers) {
        ss << k << ": " << v << "\r\n";
    }

    if (!body.empty()) {
        ss << "Content-Length: " << body.size() << "\r\n";
    }

    ss << "\r\n";
    if (!body.empty()) ss << body;

    std::string resp = ss.str();
    // write_mu already held by caller or we acquire it
    ssize_t n = ::send(c.fd, resp.data(), resp.size(), MSG_NOSIGNAL);
    if (n < 0) {
        HAL_LOG_WARNING("RTSP: send response failed for client %d: %s",
                       c.id, strerror(errno));
        c.alive = false;
    }
}

/* ================================================================
 * SDP generation
 * ================================================================ */

std::string RtspServer::build_sdp(const MediaStream& ms) {
    std::ostringstream sdp;
    sdp << "v=0\r\n";
    sdp << "o=- 0 0 IN IP4 0.0.0.0\r\n";
    sdp << "s=AIPC Camera\r\n";
    sdp << "t=0 0\r\n";
    sdp << "a=control:*\r\n";

    if (ms.info.is_audio) {
        // Standalone audio stream SDP
        uint8_t pt = RTP_AUDIO_PT;
        sdp << "m=audio 0 RTP/AVP " << (int)pt << "\r\n";
        sdp << "a=rtpmap:" << (int)pt << " L16/"
            << ms.info.sample_rate << "/" << ms.info.channels << "\r\n";
        sdp << "a=control:trackID=0\r\n";
    } else {
        // Video stream SDP — with optional audio track
        if (ms.info.codec == "h264") {
            sdp << "m=video 0 RTP/AVP " << (int)RTP_PT << "\r\n";
            sdp << "a=rtpmap:" << (int)RTP_PT << " H264/" << RTP_CLOCK << "\r\n";

            std::string fmtp = "packetization-mode=1";
            if (!ms.sps.empty()) {
                if (ms.sps.size() >= 4) {
                    char plid[8];
                    snprintf(plid, sizeof(plid), "%02X%02X%02X",
                             ms.sps[1], ms.sps[2], ms.sps[3]);
                    fmtp += ";profile-level-id=";
                    fmtp += plid;
                }
                fmtp += ";sprop-parameter-sets=";
                fmtp += base64_encode(ms.sps.data(), ms.sps.size());
                if (!ms.pps.empty()) {
                    fmtp += ",";
                    fmtp += base64_encode(ms.pps.data(), ms.pps.size());
                }
            }
            sdp << "a=fmtp:" << (int)RTP_PT << " " << fmtp << "\r\n";
        } else {
            // H265
            sdp << "m=video 0 RTP/AVP " << (int)RTP_PT << "\r\n";
            sdp << "a=rtpmap:" << (int)RTP_PT << " H265/" << RTP_CLOCK << "\r\n";

            std::string fmtp;
            if (!ms.vps.empty()) {
                fmtp += "sprop-vps=";
                fmtp += base64_encode(ms.vps.data(), ms.vps.size());
                fmtp += ";";
            }
            if (!ms.sps.empty()) {
                fmtp += "sprop-sps=";
                fmtp += base64_encode(ms.sps.data(), ms.sps.size());
                fmtp += ";";
            }
            if (!ms.pps.empty()) {
                fmtp += "sprop-pps=";
                fmtp += base64_encode(ms.pps.data(), ms.pps.size());
            }
            if (!fmtp.empty()) {
                if (fmtp.back() == ';') fmtp.pop_back();
                sdp << "a=fmtp:" << (int)RTP_PT << " " << fmtp << "\r\n";
            }
        }
        sdp << "a=control:trackID=0\r\n";

        // Add audio track if available
        if (has_audio_stream_) {
            auto audio_it = streams_.find(audio_stream_name_);
            if (audio_it != streams_.end()) {
                auto& audio_ms = audio_it->second;
                sdp << "m=audio 0 RTP/AVP " << (int)RTP_AUDIO_PT << "\r\n";
                sdp << "a=rtpmap:" << (int)RTP_AUDIO_PT << " L16/"
                    << audio_ms.info.sample_rate << "/" << audio_ms.info.channels << "\r\n";
                sdp << "a=control:trackID=1\r\n";
            }
        }
    }

    return sdp.str();
}

/* ================================================================
 * RTSP method handlers
 * ================================================================ */

void RtspServer::handle_request(Client& c, const RtspRequest& req) {
    HAL_LOG_DEBUG("RTSP [%d]: %s %s CSeq=%d",
                 c.id, req.method.c_str(), req.uri.c_str(), req.cseq);

    if (req.method == "OPTIONS")        reply_options(c, req);
    else if (req.method == "DESCRIBE")  reply_describe(c, req);
    else if (req.method == "SETUP")     reply_setup(c, req);
    else if (req.method == "PLAY")      reply_play(c, req);
    else if (req.method == "TEARDOWN")  reply_teardown(c, req);
    else if (req.method == "GET_PARAMETER") reply_get_parameter(c, req);
    else {
        send_response(c, 405, "Method Not Allowed", req.cseq, {});
    }
}

void RtspServer::reply_options(Client& c, const RtspRequest& req) {
    send_response(c, 200, "OK", req.cseq, {
        {"Public", "OPTIONS, DESCRIBE, SETUP, PLAY, TEARDOWN, GET_PARAMETER"}
    });
}

void RtspServer::reply_describe(Client& c, const RtspRequest& req) {
    std::string name = extract_stream_name(req.uri);
    auto it = streams_.find(name);
    if (it == streams_.end()) {
        send_response(c, 404, "Not Found", req.cseq, {});
        return;
    }

    std::string sdp;
    {
        std::lock_guard<std::mutex> lock(it->second.mu);
        sdp = build_sdp(it->second);
    }

    send_response(c, 200, "OK", req.cseq, {
        {"Content-Type", "application/sdp"},
        {"Content-Base", req.uri + "/"},
    }, sdp);
}

void RtspServer::reply_setup(Client& c, const RtspRequest& req) {
    std::string name = extract_stream_name(req.uri);
    if (streams_.find(name) == streams_.end()) {
        send_response(c, 404, "Not Found", req.cseq, {});
        return;
    }

    // Parse trackID from URI
    int track_id = 0;
    {
        std::string uri_path = req.uri;
        auto tid_pos = uri_path.find("trackID=");
        if (tid_pos != std::string::npos) {
            track_id = std::atoi(uri_path.c_str() + tid_pos + 8);
        }
    }

    // Parse Transport header
    auto tr_it = req.headers.find("Transport");
    if (tr_it == req.headers.end()) {
        send_response(c, 461, "Unsupported Transport", req.cseq, {});
        return;
    }

    const std::string& transport = tr_it->second;

    // We only support TCP interleaved
    if (transport.find("RTP/AVP/TCP") == std::string::npos &&
        transport.find("RTP/AVP") != std::string::npos &&
        transport.find("TCP") == std::string::npos) {
        send_response(c, 461, "Unsupported Transport", req.cseq, {});
        return;
    }

    // Parse interleaved channels
    uint8_t rtp_ch = 0, rtcp_ch = 1;
    auto il = transport.find("interleaved=");
    if (il != std::string::npos) {
        int a = 0, b = 1;
        if (sscanf(transport.c_str() + il, "interleaved=%d-%d", &a, &b) >= 1) {
            rtp_ch = (uint8_t)a;
            rtcp_ch = (uint8_t)b;
        }
    }

    c.stream_name = name;

    if (track_id == 0) {
        // Video track
        c.rtp_channel = rtp_ch;
        c.rtcp_channel = rtcp_ch;
        c.rtp_ssrc = generate_ssrc();
        c.rtp_seq = 0;
        c.ts_base_set = false;
        c.pkt_count = 0;
        c.octet_count = 0;

        // Auto-assign audio channels if multi-track capable
        if (has_audio_stream_ && !streams_.find(name)->second.info.is_audio) {
            c.has_audio = true;
            c.audio_rtp_channel = rtp_ch + 2;
            c.audio_rtcp_channel = rtp_ch + 3;
            c.audio_rtp_ssrc = generate_ssrc();
            c.audio_rtp_seq = 0;
            c.rtp_audio_sample_count = 0;
        }
    } else if (track_id == 1) {
        // Audio track (multi-track SETUP)
        c.has_audio = true;
        c.audio_rtp_channel = rtp_ch;
        c.audio_rtcp_channel = rtcp_ch;
        c.audio_rtp_ssrc = generate_ssrc();
        c.audio_rtp_seq = 0;
        c.rtp_audio_sample_count = 0;
    }

    if (c.session_id.empty()) {
        c.session_id = generate_session_id();
    }
    c.state = Client::READY;

    char ssrc_hex[16];
    uint32_t setup_ssrc = (track_id == 1) ? c.audio_rtp_ssrc : c.rtp_ssrc;
    snprintf(ssrc_hex, sizeof(ssrc_hex), "%08X", setup_ssrc);

    char transport_resp[256];
    snprintf(transport_resp, sizeof(transport_resp),
             "RTP/AVP/TCP;unicast;interleaved=%d-%d;ssrc=%s",
             rtp_ch, rtcp_ch, ssrc_hex);

    send_response(c, 200, "OK", req.cseq, {
        {"Transport", transport_resp},
        {"Session", c.session_id + ";timeout=60"},
    });

    HAL_LOG_INFO("RTSP [%d]: SETUP stream=%s trackID=%d channels=%d-%d ssrc=%s",
                c.id, name.c_str(), track_id, rtp_ch, rtcp_ch, ssrc_hex);
}

void RtspServer::reply_play(Client& c, const RtspRequest& req) {
    if (c.state != Client::READY && c.state != Client::PLAYING) {
        send_response(c, 455, "Method Not Valid in This State", req.cseq, {});
        return;
    }

    c.state = Client::PLAYING;

    // Build RTP-Info: include video track + audio track if multi-track
    std::string base_uri = req.uri;
    // Strip any existing trackID from base URI
    auto tid_pos = base_uri.find("/trackID");
    if (tid_pos != std::string::npos) base_uri = base_uri.substr(0, tid_pos);
    // Also strip trailing query params
    auto qpos = base_uri.find('?');
    if (qpos != std::string::npos) base_uri = base_uri.substr(0, qpos);

    char rtp_info[512];
    if (c.has_audio) {
        snprintf(rtp_info, sizeof(rtp_info),
                 "url=%s/trackID=0;seq=%u,url=%s/trackID=1;seq=%u",
                 base_uri.c_str(), c.rtp_seq,
                 base_uri.c_str(), c.audio_rtp_seq);
    } else {
        snprintf(rtp_info, sizeof(rtp_info),
                 "url=%s/trackID=0;seq=%u",
                 base_uri.c_str(), c.rtp_seq);
    }

    send_response(c, 200, "OK", req.cseq, {
        {"Session", c.session_id},
        {"RTP-Info", rtp_info},
        {"Range", "npt=0.000-"},
    });

    HAL_LOG_INFO("RTSP [%d]: PLAY stream=%s audio=%s",
                c.id, c.stream_name.c_str(),
                c.has_audio ? "yes" : "no");

    // Request keyframe so client can start decoding immediately
    if (keyframe_request_fn_) {
        keyframe_request_fn_(c.stream_name);
    }
}

void RtspServer::set_keyframe_request_cb(RtspServer::KeyframeRequestFn fn) {
    keyframe_request_fn_ = std::move(fn);
}

void RtspServer::set_audio_info(const std::string& audio_stream_name) {
    audio_stream_name_ = audio_stream_name;
    has_audio_stream_ = !audio_stream_name.empty();
    if (has_audio_stream_) {
        HAL_LOG_INFO("RTSP: Audio associated for multi-track (source=%s)",
                    audio_stream_name.c_str());
    }
}

void RtspServer::reply_teardown(Client& c, const RtspRequest& req) {
    send_response(c, 200, "OK", req.cseq, {
        {"Session", c.session_id},
    });

    HAL_LOG_INFO("RTSP [%d]: TEARDOWN", c.id);
    c.state = Client::INIT;
    c.alive = false;
}

void RtspServer::reply_get_parameter(Client& c, const RtspRequest& req) {
    send_response(c, 200, "OK", req.cseq, {
        {"Session", c.session_id},
    });
}

/* ================================================================
 * RTP packetization
 * ================================================================ */

void RtspServer::send_rtp_raw(Client& c, const uint8_t* payload, size_t len,
                               bool marker, uint32_t ts) {
    // Build RTP header (12 bytes) + payload
    uint8_t buf[12 + RTP_MTU + 128]; // extra room for FU header
    if (len + 12 > sizeof(buf)) {
        // Shouldn't happen for single RTP packet
        return;
    }

    buf[0] = 0x80;  // V=2, P=0, X=0, CC=0
    buf[1] = RTP_PT | (marker ? 0x80 : 0x00);
    buf[2] = (c.rtp_seq >> 8) & 0xFF;
    buf[3] = c.rtp_seq & 0xFF;
    buf[4] = (ts >> 24) & 0xFF;
    buf[5] = (ts >> 16) & 0xFF;
    buf[6] = (ts >> 8) & 0xFF;
    buf[7] = ts & 0xFF;
    buf[8]  = (c.rtp_ssrc >> 24) & 0xFF;
    buf[9]  = (c.rtp_ssrc >> 16) & 0xFF;
    buf[10] = (c.rtp_ssrc >> 8) & 0xFF;
    buf[11] = c.rtp_ssrc & 0xFF;

    memcpy(buf + 12, payload, len);

    c.rtp_seq++;
    c.pkt_count++;
    c.octet_count += (uint32_t)len;

    send_tcp_interleaved(c, c.rtp_channel, buf, 12 + len);
}

// H264 RTP: RFC 6184
// Single NAL unit mode (type 1-23) or FU-A (type 28)
void RtspServer::send_rtp_h264(Client& c, const uint8_t* nal, size_t len,
                                uint32_t ts, bool marker) {
    if (len == 0) return;

    if (len <= RTP_MTU) {
        // Single NAL unit packet
        send_rtp_raw(c, nal, len, marker, ts);
    } else {
        // FU-A fragmentation
        uint8_t nal_hdr = nal[0];
        uint8_t fu_indicator = (nal_hdr & 0xE0) | 28;  // FU-A type
        uint8_t nal_type = nal_hdr & 0x1F;

        const uint8_t* ptr = nal + 1;
        size_t remaining = len - 1;
        bool first = true;

        while (remaining > 0) {
            size_t frag_size = std::min(remaining, RTP_MTU - 2); // 2 = FU indicator + FU header
            bool last = (frag_size == remaining);

            uint8_t fu_header = nal_type;
            if (first) fu_header |= 0x80;  // S bit
            if (last)  fu_header |= 0x40;  // E bit

            uint8_t frag_buf[2 + RTP_MTU];
            frag_buf[0] = fu_indicator;
            frag_buf[1] = fu_header;
            memcpy(frag_buf + 2, ptr, frag_size);

            send_rtp_raw(c, frag_buf, 2 + frag_size,
                        last && marker, ts);

            ptr += frag_size;
            remaining -= frag_size;
            first = false;
        }
    }
}

// H265 RTP: RFC 7798
// Single NAL unit mode or FU (type 49)
void RtspServer::send_rtp_h265(Client& c, const uint8_t* nal, size_t len,
                                uint32_t ts, bool marker) {
    if (len < 2) return;

    if (len <= RTP_MTU) {
        // Single NAL unit packet
        send_rtp_raw(c, nal, len, marker, ts);
    } else {
        // FU fragmentation
        uint8_t nal_hdr0 = nal[0];
        uint8_t nal_hdr1 = nal[1];
        uint8_t nal_type = (nal_hdr0 >> 1) & 0x3F;

        // PayloadHdr: type=49 (FU), keep layerID and TID from original
        uint8_t ph0 = (nal_hdr0 & 0x81) | (49 << 1);
        uint8_t ph1 = nal_hdr1;

        const uint8_t* ptr = nal + 2;
        size_t remaining = len - 2;
        bool first = true;

        while (remaining > 0) {
            size_t frag_size = std::min(remaining, RTP_MTU - 3); // 3 = PayloadHdr(2) + FU header(1)
            bool last = (frag_size == remaining);

            uint8_t fu_header = nal_type;
            if (first) fu_header |= 0x80;  // S bit
            if (last)  fu_header |= 0x40;  // E bit

            uint8_t frag_buf[3 + RTP_MTU];
            frag_buf[0] = ph0;
            frag_buf[1] = ph1;
            frag_buf[2] = fu_header;
            memcpy(frag_buf + 3, ptr, frag_size);

            send_rtp_raw(c, frag_buf, 3 + frag_size,
                        last && marker, ts);

            ptr += frag_size;
            remaining -= frag_size;
            first = false;
        }
    }
}

/* ================================================================
 * TCP interleaved transport
 * ================================================================ */

void RtspServer::send_tcp_interleaved(Client& c, uint8_t channel,
                                       const uint8_t* data, size_t len) {
    if (len > 0xFFFF) return;

    // '$' + channel(1) + length(2, big-endian) + data
    uint8_t hdr[4];
    hdr[0] = '$';
    hdr[1] = channel;
    hdr[2] = (len >> 8) & 0xFF;
    hdr[3] = len & 0xFF;

    // Use writev-like approach: send header + data
    // write_mu must be held by caller
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = 4;
    iov[1].iov_base = const_cast<uint8_t*>(data);
    iov[1].iov_len = len;

    struct msghdr msg{};
    msg.msg_iov = iov;
    msg.msg_iovlen = 2;

    ssize_t sent = ::sendmsg(c.fd, &msg, MSG_NOSIGNAL);
    if (sent < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Buffer full — drop immediately to avoid blocking encoder callback
            return;
        }
        c.alive = false;
    }
}

/* ================================================================
 * Network: server loop (epoll)
 * ================================================================ */

RtspServer::RtspServer() = default;

RtspServer::~RtspServer() {
    stop();
}

void RtspServer::add_stream(const StreamInfo& info) {
    streams_.emplace(std::piecewise_construct,
                     std::forward_as_tuple(info.name),
                     std::forward_as_tuple());
    streams_.at(info.name).info = info;
}

bool RtspServer::start(uint16_t port) {
    port_ = port;

    // Create listen socket
    listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd_ < 0) {
        HAL_LOG_ERROR("RTSP: socket() failed: %s", strerror(errno));
        return false;
    }

    int yes = 1;
    setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    set_nonblocking(listen_fd_);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (::bind(listen_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        HAL_LOG_ERROR("RTSP: bind(%d) failed: %s", port, strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    if (::listen(listen_fd_, 8) < 0) {
        HAL_LOG_ERROR("RTSP: listen() failed: %s", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    // Create epoll
    epoll_fd_ = ::epoll_create1(0);
    if (epoll_fd_ < 0) {
        HAL_LOG_ERROR("RTSP: epoll_create1() failed: %s", strerror(errno));
        ::close(listen_fd_);
        listen_fd_ = -1;
        return false;
    }

    struct epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u64 = UINT64_MAX;  // Sentinel for listen socket
    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, listen_fd_, &ev);

    running_.store(true);
    server_thread_ = std::thread(&RtspServer::server_loop, this);

    HAL_LOG_INFO("RTSP: Server started on port %d", port);

    // Log available streams
    for (auto& [name, ms] : streams_) {
        HAL_LOG_INFO("RTSP:   rtsp://<device>:%d/%s (%s %ux%u@%u)",
                    port, name.c_str(), ms.info.codec.c_str(),
                    ms.info.width, ms.info.height, ms.info.fps);
    }

    return true;
}

void RtspServer::stop() {
    if (!running_.exchange(false)) return;

    HAL_LOG_INFO("RTSP: Stopping server...");

    // Wake up epoll by closing listen socket
    if (listen_fd_ >= 0) {
        ::shutdown(listen_fd_, SHUT_RDWR);
        ::close(listen_fd_);
        listen_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    // Close all clients
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& [id, c] : clients_) {
            if (c->fd >= 0) ::close(c->fd);
        }
        clients_.clear();
    }

    if (epoll_fd_ >= 0) {
        ::close(epoll_fd_);
        epoll_fd_ = -1;
    }

    HAL_LOG_INFO("RTSP: Server stopped");
}

void RtspServer::server_loop() {
    constexpr int MAX_EVENTS = 32;
    struct epoll_event events[MAX_EVENTS];

    while (running_.load()) {
        int n = ::epoll_wait(epoll_fd_, events, MAX_EVENTS, 500);
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }

        for (int i = 0; i < n; i++) {
            if (events[i].data.u64 == UINT64_MAX) {
                // Accept new connections on listen socket
                while (true) {
                    struct sockaddr_in client_addr{};
                    socklen_t addrlen = sizeof(client_addr);
                    int cfd = ::accept(listen_fd_,
                                      (struct sockaddr*)&client_addr,
                                      &addrlen);
                    if (cfd < 0) break;

                    set_nonblocking(cfd);
                    int flag = 1;
                    setsockopt(cfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

                    // Set send buffer (2MB for video streaming at 30fps)
                    int sndbuf = 2 * 1024 * 1024;
                    setsockopt(cfd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

                    auto client = std::make_unique<Client>();
                    client->fd = cfd;

                    int cid;
                    {
                        std::lock_guard<std::mutex> lock(clients_mu_);
                        cid = next_client_id_++;
                        client->id = cid;
                        clients_[cid] = std::move(client);
                    }

                    struct epoll_event cev{};
                    cev.events = EPOLLIN;
                    cev.data.u64 = (uint64_t)cid;
                    epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, cfd, &cev);

                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
                    HAL_LOG_INFO("RTSP [%d]: Connected from %s:%d",
                                cid, ip, ntohs(client_addr.sin_port));
                }
            } else {
                // Client data
                handle_client_data((int)events[i].data.u64);
            }
        }

        // Reap dead clients
        std::vector<int> dead;
        {
            std::lock_guard<std::mutex> lock(clients_mu_);
            for (auto& [id, c] : clients_) {
                if (!c->alive) dead.push_back(id);
            }
        }
        for (int id : dead) {
            remove_client(id);
        }
    }
}

void RtspServer::handle_client_data(int client_id) {
    Client* c = nullptr;
    {
        std::lock_guard<std::mutex> lock(clients_mu_);
        auto it = clients_.find(client_id);
        if (it == clients_.end()) return;
        c = it->second.get();
    }

    // 1. Read all available data (non-blocking)
    char buf[4096];
    while (true) {
        ssize_t n = ::recv(c->fd, buf, sizeof(buf), 0);
        if (n > 0) {
            c->read_buf.append(buf, (size_t)n);
        } else if (n == 0) {
            HAL_LOG_INFO("RTSP [%d]: Disconnected (EOF)", c->id);
            c->alive = false;
            return;
        } else {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;  // All available data read, proceed to process
            }
            HAL_LOG_INFO("RTSP [%d]: recv error: %s", c->id, strerror(errno));
            c->alive = false;
            return;
        }
    }

    // 2. Process buffered data: skip interleaved '$' frames, parse RTSP requests
    while (!c->read_buf.empty()) {
        // Skip TCP interleaved data from client (e.g., RTCP RR)
        if ((uint8_t)c->read_buf[0] == '$') {
            if (c->read_buf.size() < 4) break;  // Need more data
            uint16_t il_len = ((uint8_t)c->read_buf[2] << 8) |
                               (uint8_t)c->read_buf[3];
            if (c->read_buf.size() < 4u + il_len) break;  // Incomplete
            c->read_buf.erase(0, 4 + il_len);
            continue;
        }

        // Look for complete RTSP request (\r\n\r\n)
        auto end = c->read_buf.find("\r\n\r\n");
        if (end == std::string::npos) break;  // Need more data

        std::string raw = c->read_buf.substr(0, end + 4);
        c->read_buf.erase(0, end + 4);

        RtspRequest req;
        if (parse_request(raw, req)) {
            std::lock_guard<std::mutex> lock(c->write_mu);
            handle_request(*c, req);
        }
    }
}

void RtspServer::remove_client(int client_id) {
    std::lock_guard<std::mutex> lock(clients_mu_);
    auto it = clients_.find(client_id);
    if (it == clients_.end()) return;

    int fd = it->second->fd;
    if (fd >= 0) {
        epoll_ctl(epoll_fd_, EPOLL_CTL_DEL, fd, nullptr);
        ::close(fd);
    }

    HAL_LOG_INFO("RTSP [%d]: Removed", client_id);
    clients_.erase(it);
}

/* ================================================================
 * Packet input (from encoder callback, thread-safe)
 * ================================================================ */

void RtspServer::on_packet(const std::string& stream_name,
                            const HalPacketBuffer* packet) {
    if (!running_.load()) return;
    if (!packet || !packet->data || packet->size == 0) return;

    // Find stream
    auto ms_it = streams_.find(stream_name);
    if (ms_it == streams_.end()) return;
    auto& ms = ms_it->second;

    // Audio stream: send raw PCM as RTP L16 packets
    if (ms.info.is_audio) {
        // Byte-swap S16LE → BE for RTP L16 (RFC 3190)
        std::vector<uint8_t> be_data(packet->size);
        const uint8_t* src = packet->data;
        uint8_t* dst = be_data.data();
        for (size_t i = 0; i + 1 < packet->size; i += 2) {
            dst[i]     = src[i + 1];  // swap low/high byte
            dst[i + 1] = src[i];
        }

        uint32_t samples = (uint32_t)(packet->size / (ms.info.channels * 2));

        std::lock_guard<std::mutex> lock(clients_mu_);
        for (auto& [id, c] : clients_) {
            if (c->state != Client::PLAYING) continue;
            if (!c->alive) continue;

            // Send audio to:
            // 1. Clients directly subscribed to this audio stream
            // 2. Clients subscribed to video streams with has_audio=true (multi-track)
            bool direct_subscriber = (c->stream_name == stream_name);
            bool multi_track_subscriber = (c->has_audio && !direct_subscriber);

            if (!direct_subscriber && !multi_track_subscriber) continue;

            uint32_t audio_ts = c->rtp_audio_sample_count;
            c->rtp_audio_sample_count += samples;

            // Send PCM in RTP chunks (max ~1400 bytes per packet)
            const uint8_t* ptr = be_data.data();
            size_t remaining = be_data.size();
            while (remaining > 0) {
                size_t chunk = std::min(remaining, (size_t)RTP_MTU);
                bool last = (chunk == remaining);

                uint8_t buf[12 + RTP_MTU];
                buf[0] = 0x80;
                buf[1] = RTP_AUDIO_PT | (last ? 0x80 : 0x00);

                if (direct_subscriber) {
                    // Standalone audio: use video-track seq/ssrc/channel
                    buf[2] = (c->rtp_seq >> 8) & 0xFF;
                    buf[3] = c->rtp_seq & 0xFF;
                    buf[8]  = (c->rtp_ssrc >> 24) & 0xFF;
                    buf[9]  = (c->rtp_ssrc >> 16) & 0xFF;
                    buf[10] = (c->rtp_ssrc >> 8) & 0xFF;
                    buf[11] = c->rtp_ssrc & 0xFF;
                } else {
                    // Multi-track: use dedicated audio seq/ssrc/channel
                    buf[2] = (c->audio_rtp_seq >> 8) & 0xFF;
                    buf[3] = c->audio_rtp_seq & 0xFF;
                    buf[8]  = (c->audio_rtp_ssrc >> 24) & 0xFF;
                    buf[9]  = (c->audio_rtp_ssrc >> 16) & 0xFF;
                    buf[10] = (c->audio_rtp_ssrc >> 8) & 0xFF;
                    buf[11] = c->audio_rtp_ssrc & 0xFF;
                }
                buf[4] = (audio_ts >> 24) & 0xFF;
                buf[5] = (audio_ts >> 16) & 0xFF;
                buf[6] = (audio_ts >> 8) & 0xFF;
                buf[7] = audio_ts & 0xFF;

                memcpy(buf + 12, ptr, chunk);

                if (direct_subscriber) {
                    c->rtp_seq++;
                    c->pkt_count++;
                    c->octet_count += (uint32_t)chunk;
                } else {
                    c->audio_rtp_seq++;
                }

                {
                    std::lock_guard<std::mutex> wlock(c->write_mu);
                    uint8_t ch = direct_subscriber ? c->rtp_channel : c->audio_rtp_channel;
                    send_tcp_interleaved(*c, ch, buf, 12 + chunk);
                }

                ptr += chunk;
                remaining -= chunk;
            }
        }
        return;
    }

    // Video stream
    bool is_h265 = (ms.info.codec == "h265");

    // Copy packet data to local buffer to minimize time holding media library buffer
    std::vector<uint8_t> local_data(packet->data, packet->data + packet->size);
    // v2: keyframe detected from NAL parsing below
    bool is_keyframe = false;
    uint64_t timestamp_ns = packet->timestamp_ns;

    // Parse NAL units from local copy
    auto nals = split_nals(local_data.data(), local_data.size());
    if (nals.empty()) return;

    // Extract parameter sets from ANY packet that contains SPS/PPS NALs.
    // Don't rely on is_keyframe flag alone — some HAL implementations may not
    // set it correctly, and SPS/PPS can also appear in non-IDR access units.
    {
        std::lock_guard<std::mutex> lock(ms.mu);
        for (auto& nal : nals) {
            if (nal.size == 0) continue;
            if (is_h265) {
                uint8_t type = h265_nal_type(nal.data);
                if (type == 32) ms.vps.assign(nal.data, nal.data + nal.size);
                else if (type == 33) ms.sps.assign(nal.data, nal.data + nal.size);
                else if (type == 34) ms.pps.assign(nal.data, nal.data + nal.size);
            } else {
                uint8_t type = h264_nal_type(nal.data);
                if (type == 7) ms.sps.assign(nal.data, nal.data + nal.size);
                else if (type == 8) ms.pps.assign(nal.data, nal.data + nal.size);
            }
        }
        if (!ms.sps.empty()) ms.params_ready = true;
    }

    // Distribute to all PLAYING clients for this stream
    std::lock_guard<std::mutex> lock(clients_mu_);
    for (auto& [id, c] : clients_) {
        if (c->state != Client::PLAYING) continue;
        if (c->stream_name != stream_name) continue;
        if (!c->alive) continue;

        // RTP timestamp: use monotonic frame counter instead of HAL timestamp.
        // HAL timestamps may be non-monotonic (encoder reordering), which
        // causes jitter buffers in players to stall.
        // Each on_packet call = one access unit = one RTP timestamp tick.
        uint32_t ts_increment = (ms.info.fps > 0) ? (RTP_CLOCK / ms.info.fps) : 3000;
        uint32_t rel_ts = c->rtp_frame_count * ts_increment;
        c->rtp_frame_count++;

        std::lock_guard<std::mutex> wlock(c->write_mu);

        for (size_t i = 0; i < nals.size(); i++) {
            if (!c->alive) break;
            bool last_nal = (i == nals.size() - 1);
            if (is_h265)
                send_rtp_h265(*c, nals[i].data, nals[i].size,
                             rel_ts, last_nal);
            else
                send_rtp_h264(*c, nals[i].data, nals[i].size,
                             rel_ts, last_nal);
        }
    }
}
