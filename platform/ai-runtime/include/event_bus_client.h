#pragma once

#include <string>
#include <memory>
#include <mutex>

#include <grpcpp/grpcpp.h>
#include "event.grpc.pb.h"

namespace aipc::ai_runtime {

class EventBusClient {
public:
    /// Connect to Event Bus. Returns false on failure.
    bool connect(const std::string& endpoint, int timeout_ms = 5000);

    /// Publish an inference result event.
    bool publish(const std::string& topic,
                 const std::string& source,
                 uint64_t timestamp_ns,
                 const std::string& event_id,
                 const std::string& payload_json,
                 const std::map<std::string, std::string>& metadata = {});

    void disconnect();
    bool connected() const;

private:
    std::shared_ptr<grpc::Channel>                      channel_;
    std::unique_ptr<aipc::event::EventBus::Stub>        stub_;
    mutable std::mutex mu_;
};

}  // namespace aipc::ai_runtime
