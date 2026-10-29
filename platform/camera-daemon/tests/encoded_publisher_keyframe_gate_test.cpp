#include "encoded_publisher.h"

#include <cassert>
#include <cerrno>
#include <chrono>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace {

bool read_exact(int fd, void* data, size_t size) {
    auto* out = static_cast<uint8_t*>(data);
    size_t total = 0;
    while (total < size) {
        const ssize_t n = ::recv(fd, out + total, size - total, 0);
        if (n <= 0) return false;
        total += static_cast<size_t>(n);
    }
    return true;
}

void publish(EncodedPublisher& publisher, const uint8_t* data, size_t size) {
    HalPacketBuffer packet{};
    packet.data = const_cast<uint8_t*>(data);
    packet.size = static_cast<uint32_t>(size);
    packet.timestamp_ns = 1;
    publisher.on_packet("main", &packet);
}

}  // namespace

int main() {
    char dir_template[] = "/tmp/encoded-publisher-keyframe-gate.XXXXXX";
    char* dir = ::mkdtemp(dir_template);
    assert(dir != nullptr);

    EncodedPublisher publisher;
    EncodedPublisher::StreamConfig config;
    config.name = "main";
    config.codec = "h264";
    config.width = 1920;
    config.height = 1080;
    publisher.add_stream(config, dir);

    std::atomic<int> keyframe_requests{0};
    publisher.set_keyframe_request_cb(
        [&](const std::string& stream_name) {
            assert(stream_name == "main");
            ++keyframe_requests;
        });
    assert(publisher.start());

    const std::string socket_path = std::string(dir) + "/main.sock";
    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    assert(client >= 0);

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, socket_path.c_str(), sizeof(address.sun_path) - 1);
    assert(::connect(client, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);

    // Wait (bounded poll) for the accept loop to install the new client
    // before publishing - a fixed sleep flakes under scheduler load.
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        EncodedPublisher::StreamDropStats stats{};
        bool client_installed = false;
        while (!client_installed &&
               std::chrono::steady_clock::now() < deadline) {
            client_installed =
                publisher.get_stream_stats("main", &stats) && stats.clients == 1;
            if (!client_installed) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
        assert(client_installed);
    }

    const uint8_t p_frame[] = {0x00, 0x00, 0x00, 0x01, 0x41, 0x01};
    publish(publisher, p_frame, sizeof(p_frame));

    pollfd wait_for_p{client, POLLIN, 0};
    assert(::poll(&wait_for_p, 1, 200) == 0);
    // Keyframe dispatch is asynchronous (dedicated worker); poll for it.
    {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::seconds(2);
        while (keyframe_requests.load() == 0 &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        assert(keyframe_requests.load() > 0);
    }

    const uint8_t idr_frame[] = {0x00, 0x00, 0x00, 0x01, 0x65, 0x88};
    publish(publisher, idr_frame, sizeof(idr_frame));

    pollfd wait_for_idr{client, POLLIN, 0};
    assert(::poll(&wait_for_idr, 1, 1000) == 1);

    uint8_t header[EncodedPublisher::HEADER_SIZE]{};
    assert(read_exact(client, header, sizeof(header)));
    assert((header[5] & EncodedPublisher::FLAG_KEYFRAME) != 0);

    uint32_t total_size = 0;
    std::memcpy(&total_size, header, sizeof(total_size));
    assert(total_size == EncodedPublisher::HEADER_SIZE + sizeof(idr_frame));

    uint8_t payload[sizeof(idr_frame)]{};
    assert(read_exact(client, payload, sizeof(payload)));
    assert(std::memcmp(payload, idr_frame, sizeof(idr_frame)) == 0);

    ::close(client);

    // Runtime layout changes happen while encoder callbacks and both worker
    // threads are live. Exercise replacement/insertion/removal concurrently;
    // every StreamState dereference must remain valid until its operation
    // completes (the old unprotected streams_ map was a UAF/data race here).
    std::atomic<bool> keep_publishing{true};
    std::thread producer([&] {
        while (keep_publishing.load()) {
            publish(publisher, idr_frame, sizeof(idr_frame));
            std::this_thread::yield();
        }
    });
    for (uint32_t i = 0; i < 40; ++i) {
        config.width = 1920 + i;
        publisher.add_stream(config, dir);

        EncodedPublisher::StreamConfig aux{};
        aux.name = "aux";
        aux.codec = "h265";
        aux.width = 640;
        aux.height = 360;
        publisher.add_stream(aux, dir);
        assert(publisher.has_stream("aux"));
        publisher.remove_stream("aux");

        EncodedPublisher::StreamDropStats stats{};
        assert(publisher.get_stream_stats("main", &stats));
        assert(!publisher.stream_names().empty());
    }
    keep_publishing.store(false);
    producer.join();

    publisher.stop();
    ::unlink(socket_path.c_str());
    ::rmdir(dir);
    return 0;
}
