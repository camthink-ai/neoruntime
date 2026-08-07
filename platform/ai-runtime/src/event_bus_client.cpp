#include "event_bus_client.h"
#include "log.h"
#include <chrono>
#include <cstdlib>
#include <cstring>

#include <grpcpp/create_channel_posix.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

namespace aipc::ai_runtime {

// Hailo SDK gRPC 1.46 + musl libc: all resolvers broken.
// Bypass by creating raw socket and handing fd to gRPC.
static std::string extract_unix_path(const std::string& ep) {
    if (ep.rfind("unix:///", 0) == 0) return ep.substr(7);
    if (ep.rfind("unix:", 0) == 0)    return ep.substr(5);
    if (!ep.empty() && ep[0] == '/')  return ep;
    return {};
}

static std::shared_ptr<grpc::Channel> connect_endpoint(const std::string& endpoint) {
    // --- Unix domain socket ---
    std::string upath = extract_unix_path(endpoint);
    if (!upath.empty()) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return nullptr;
        struct sockaddr_un sa{};
        sa.sun_family = AF_UNIX;
        strncpy(sa.sun_path, upath.c_str(), sizeof(sa.sun_path) - 1);
        if (::connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
            close(fd);
            return nullptr;
        }
        return grpc::CreateInsecureChannelFromFd("event-bus", fd);
    }

    // --- TCP (ip:port or ipv4:ip:port) ---
    std::string addr = endpoint;
    if (addr.rfind("ipv4:", 0) == 0) addr = addr.substr(5);

    auto colon = addr.rfind(':');
    if (colon == std::string::npos) return nullptr;
    std::string host = addr.substr(0, colon);
    int port = std::atoi(addr.substr(colon + 1).c_str());
    if (host.empty() || port <= 0) return nullptr;

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return nullptr;
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &sa.sin_addr) != 1) {
        close(fd);
        return nullptr;
    }
    if (::connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        close(fd);
        return nullptr;
    }
    return grpc::CreateInsecureChannelFromFd("event-bus", fd);
}

bool EventBusClient::connect(const std::string& endpoint, int timeout_ms) {
    std::lock_guard lock(mu_);

    channel_ = connect_endpoint(endpoint);
    if (!channel_) {
        LOG_WARN("EventBusClient: connect to %s failed", endpoint.c_str());
        return false;
    }

    stub_ = aipc::event::EventBus::NewStub(channel_);
    LOG_INFO("EventBusClient: connected to %s", endpoint.c_str());
    return true;
}

bool EventBusClient::publish(const std::string& topic,
                             const std::string& source,
                             uint64_t timestamp_ns,
                             const std::string& event_id,
                             const std::string& payload_json,
                             const std::map<std::string, std::string>& metadata) {
    std::lock_guard lock(mu_);
    if (!stub_) return false;

    aipc::event::PublishRequest req;
    auto* evt = req.mutable_event();
    evt->set_topic(topic);
    evt->set_source(source);
    evt->set_timestamp_ns(timestamp_ns);
    evt->set_event_id(event_id);
    evt->set_payload(payload_json);
    evt->set_payload_type("json");

    for (auto& [k, v] : metadata) {
        (*evt->mutable_metadata())[k] = v;
    }

    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(500));

    aipc::event::PublishResponse resp;
    auto status = stub_->Publish(&ctx, req, &resp);

    if (!status.ok()) {
        LOG_DEBUG("EventBusClient: Publish failed: %s", status.error_message().c_str());
        return false;
    }
    return true;
}

void EventBusClient::disconnect() {
    std::lock_guard lock(mu_);
    stub_.reset();
    channel_.reset();
}

bool EventBusClient::connected() const {
    std::lock_guard lock(mu_);
    return stub_ != nullptr;
}

}  // namespace aipc::ai_runtime
