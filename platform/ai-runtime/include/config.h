#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace aipc::ai_runtime {

struct PreloadModel {
    std::string id;
    std::string path;
    std::string type;
    std::string postprocess_json;  // Initial postprocess JSON config (e.g., CLIP prompts)
};

struct AutoInferPipeline {
    std::string model_id;
    std::string stream_id;
    uint32_t    fps = 10;
};

struct Config {
    // Service
    std::string service_name    = "ai-runtime";
    std::string listen_address  = "unix:///run/aipc/ai-runtime.sock";
    std::string log_level       = "info";
    std::string log_file;

    // HAL
    std::string hal_library_path = "/data/aipc/lib/hal/libaipc_hal.so";
    std::string hal_device_path;
    std::string hal_platform_config;  // hal.platform_config passthrough; HAL only reads
                                       // backend_function (device_id is stored, unused)

    // Models
    std::string model_repository_path = "/data/aipc/models";
    std::string model_cache_path;
    std::vector<PreloadModel> preload_models;

    // Scheduler
    uint32_t scheduler_workers          = 4;
    uint32_t scheduler_queue_size       = 64;
    uint32_t scheduler_timeout_ms       = 5000;
    uint32_t global_qps_limit           = 100;
    uint32_t default_session_max_qps    = 30;
    uint32_t default_session_priority   = 5;

    // Postprocess pool (async post-processing offload)
    uint32_t postprocess_workers    = 2;
    uint32_t postprocess_queue_size = 32;

    // FD Receiver (zero-copy DMA-BUF)
    std::string fd_socket_path = "/run/aipc/camera.sock";

    // Stream DSP preprocess: when a StreamInfer frame's geometry does not
    // match the model input, resize it on the DSP into a private model-geometry
    // dmabuf pool and fd-bind that (zero CPU copies). Opt-in; when disabled or
    // unavailable, the existing direct DMA path retains HAL size validation.
    bool     stream_dsp_preprocess    = false;
    uint32_t stream_preprocess_slots  = 4;     // pool buffers per stream
    uint32_t stream_preprocess_job_ms = 100;   // DSP job wait timeout
    // camera-daemon CameraControl gRPC endpoint (SubmitDspJob job plane)
    std::string stream_preprocess_job_endpoint = "unix:///run/aipc/camera-control.sock";

    // Performance
    std::string device_mode = "high";

    // Event Bus
    bool        event_bus_enabled             = true;
    std::string event_bus_endpoint            = "unix:///run/aipc/event-bus.sock";
    bool        event_bus_auto_publish        = true;
    std::string event_bus_result_topic_prefix = "inference/";

    // Auto-inference (no external client needed)
    bool auto_infer_enabled = false;
    std::vector<AutoInferPipeline> auto_infer_pipelines;
};

Config load_config(const std::string& path);

// Extract the Unix socket path from "unix:///path" format
std::string parse_unix_address(const std::string& addr);

}  // namespace aipc::ai_runtime
