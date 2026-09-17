#include "config.h"
#include "log.h"
#include "hal_ml_loader.h"
#include "model_manager.h"
#include "session_manager.h"
#include "inference_scheduler.h"
#include "postprocess_pool.h"
#include "fd_receiver.h"
#include "event_bus_client.h"
#include "grpc_service.h"
#include "dsp_client.h"
#include "auto_infer.h"

#include <grpcpp/grpcpp.h>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <thread>
#include <iostream>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>

extern "C" {
#include "hal_log.h"
}

static std::atomic<bool> g_shutdown{false};
static grpc::Server* g_server = nullptr;

// AIPC group GID for socket permissions
static const gid_t AIPC_GROUP_GID = 1001;

static void signal_handler(int /*sig*/) {
    g_shutdown = true;
    // Do NOT call Shutdown() directly in signal handler!
    // It causes mutex deadlock because Wait() holds internal mutex.
    // The main loop will detect g_shutdown and call Shutdown() safely.
}

int main(int argc, char* argv[]) {
    using namespace aipc::ai_runtime;

    // Parse arguments
    std::string config_path = "/data/aipc/etc/ai-runtime.yaml";
    for (int i = 1; i < argc; i++) {
        if ((std::strcmp(argv[i], "-config") == 0 ||
             std::strcmp(argv[i], "--config") == 0) && i + 1 < argc) {
            config_path = argv[++i];
        }
    }

    // Load config
    Config cfg = load_config(config_path);
    set_log_level(cfg.log_level);

    // Remap /var/log/aipc/<name> → <prefix>/logs/<name>
    // On embedded devices /var/log is tmpfs (lost on reboot), so redirect
    // to the persistent install prefix (e.g. /data/logs/).
    std::string resolved_log_file = cfg.log_file;
    {
        const std::string var_log_prefix = "/var/log/aipc/";
        if (cfg.log_file.compare(0, var_log_prefix.size(), var_log_prefix) == 0) {
            std::string file_name = cfg.log_file.substr(var_log_prefix.size());
            std::string prefix = "/opt/aipc";
            if (config_path.find("/data/") == 0 || config_path.find("/data\\") == 0)
                prefix = "/data";
            resolved_log_file = prefix + "/logs/" + file_name;
        }
    }

    // Configure log file output if specified (via HAL log interface)
    if (!resolved_log_file.empty()) {
        // Ensure log directory exists
        std::string log_dir = resolved_log_file;
        size_t last_slash = log_dir.find_last_of('/');
        if (last_slash != std::string::npos) {
            log_dir = log_dir.substr(0, last_slash);
            mkdir(log_dir.c_str(), 0755);
        }
        // Enable file logging with rotation (10MB max, 5 files)
        hal_log_set_file(1, resolved_log_file.c_str(), 10 * 1024 * 1024, 5);
        LOG_INFO("Logging to file: %s", resolved_log_file.c_str());
    }

    LOG_INFO("Starting %s (C++)", cfg.service_name.c_str());
    LOG_INFO("Config: %s", config_path.c_str());

    // ── Load HAL ML ──────────────────────────────────────────────────────────
    HalMlLoader hal_loader;
    if (!hal_loader.load(cfg.hal_library_path)) {
        LOG_FATAL("Cannot load HAL ML: %s", cfg.hal_library_path.c_str());
        return 1;
    }

    // ── Create core components ──────────────────────────────────────────────
    ModelManager     model_mgr(hal_loader.infer_ops(), hal_loader.post_ops(),
                               hal_loader.draw_ops(), &hal_loader,
                               cfg.hal_platform_config);
    SessionManager   session_mgr;
    InferenceScheduler scheduler(&model_mgr, &session_mgr,
                                 static_cast<int>(cfg.scheduler_workers),
                                 static_cast<int>(cfg.scheduler_queue_size));
    scheduler.start();

    PostprocessPool postprocess_pool(
        static_cast<int>(cfg.postprocess_workers),
        static_cast<int>(cfg.postprocess_queue_size));
    postprocess_pool.start();

    // ── Preload models ───────────────────────────────────────────────────────
    for (auto& pm : cfg.preload_models) {
        LOG_INFO("Preloading model: %s -> %s (type=%s)", pm.id.c_str(), pm.path.c_str(), pm.type.c_str());
        // Register the same decoding identity the gRPC path carries. This
        // keeps config-preloaded entries compatible with an identical later
        // co-owner registration while rejecting a same-id/path request that
        // would rewire their postprocess session to a different type/config.
        int rc = model_mgr.register_model(pm.id, pm.path, "<system>", false,
                                          pm.postprocess_json, pm.type);
        if (rc < 0) {
            LOG_WARN("Failed to preload model %s: %d", pm.id.c_str(), rc);
            continue;
        }
        if (!pm.type.empty() && model_mgr.has_post_ops()) {
            model_mgr.init_post_process(pm.id, pm.type, pm.postprocess_json);
        }
        if (!pm.postprocess_json.empty() && model_mgr.has_post_ops()) {
            model_mgr.update_postprocess_config(pm.id, pm.postprocess_json);
            LOG_INFO("Applied postprocess config for %s", pm.id.c_str());
        }
    }

    // ── FD Receiver (zero-copy DMA-BUF from camera-daemon) ───────────────────
    FdReceiver fd_receiver(cfg.fd_socket_path);

    // ── Stream DSP preprocess: camera.sock buffer plane + camera-control
    // gRPC job plane. Process-lifetime singleton: its UDS connection owns every
    // DSP buffer id this runtime allocates/imports (disconnect cleanup is the
    // safety net), and late StreamPreprocessPool leases may outlive a stream.
    DspClient dsp_client(cfg.fd_socket_path,
                         cfg.stream_preprocess_job_endpoint);

    // ── Event Bus client ─────────────────────────────────────────────────────
    EventBusClient event_bus;
    if (cfg.event_bus_enabled && !cfg.event_bus_endpoint.empty()) {
        if (!event_bus.connect(cfg.event_bus_endpoint)) {
            LOG_WARN("Event Bus connect failed, continuing without event publishing");
        }
    }

    // Resolves Tensor.buffer_id inference inputs against camera-daemon's DSP
    // buffer registry (same UDS). Lazily connects on first use.
    BufferLookupClient buffer_lookup(cfg.fd_socket_path);

    // ── gRPC server ──────────────────────────────────────────────────────────
    AIRuntimeServiceImpl service(cfg, &model_mgr, &session_mgr,
                                 &scheduler, &fd_receiver, &buffer_lookup, &event_bus,
                                 &postprocess_pool, &dsp_client,
                                 hal_loader.clip_text_enc_ops(),
                                 hal_loader.genai_ops());

    std::string listen_addr = parse_unix_address(cfg.listen_address);

    // Remove stale socket file
    ::unlink(listen_addr.c_str());

    grpc::ServerBuilder builder;
    builder.AddListeningPort("unix:" + listen_addr,
                             grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    // Tune for low-latency edge scenario
    builder.SetMaxReceiveMessageSize(64 * 1024 * 1024);  // 64MB for large tensors
    builder.SetMaxSendMessageSize(64 * 1024 * 1024);

    auto server = builder.BuildAndStart();
    if (!server) {
        LOG_FATAL("Failed to start gRPC server on %s", listen_addr.c_str());
        return 1;
    }
    g_server = server.get();

    // Set socket permissions for container access (GID 1001 = aipc group)
    if (chmod(listen_addr.c_str(), 0660) == 0) {
        if (chown(listen_addr.c_str(), -1, AIPC_GROUP_GID) != 0) {
            LOG_WARN("Failed to set socket group ownership: %s (containers may not connect)", strerror(errno));
        } else {
            LOG_INFO("Socket permissions set for container access: %s", listen_addr.c_str());
        }
    } else {
        LOG_WARN("Failed to set socket permissions: %s", strerror(errno));
    }

    LOG_INFO("Server listening on unix://%s", listen_addr.c_str());

    // ── Auto-inference pipelines ─────────────────────────────────────────────
    AutoInfer auto_infer(&model_mgr, &fd_receiver, &event_bus, &scheduler,
                          &session_mgr, &postprocess_pool, cfg);
    // Declared before the AutoInfer startup check so the ordered-shutdown
    // helper below can join it on every exit path; assigned once Wait() starts.
    std::thread server_thread;

    // ── Ordered shutdown, shared by every exit path ──────────────────────────
    // The gRPC server is already accepting requests when AutoInfer starts, so
    // a startup failure can leave live async work. Run the same sequence as a
    // signal shutdown: stop producers first, then quiesce async HAL callbacks
    // (which own raw scheduler/service dependencies); if the bounded drain
    // cannot quiesce them, terminate without running destructors rather than
    // risk a use-after-free.
    auto shutdown_runtime = [&](int exit_code) -> int {
        server->Shutdown();
        if (server_thread.joinable()) server_thread.join();
        // 1. Stop auto-infer pipelines (no new frames submitted)
        auto_infer.stop();
        // 2. Stop scheduler workers (drain queued tasks, stop worker threads).
        //    Async HAL callbacks for in-flight jobs may still fire after this.
        scheduler.stop();
        // 3. Bounded drain of all tracked asynchronous inference work. Scheduler
        //    requests decrement the tracker from their HAL callbacks; InferBatch
        //    extends the same tracker through completion of its postprocess task.
        int orphaned = scheduler.drain_async(5000);
        if (orphaned > 0) {
            LOG_FATAL("Shutdown: %d scheduler async job(s) still in-flight; "
                      "terminating without destroying callback dependencies",
                      orphaned);
            // A late callback owns raw scheduler/service dependencies. Running
            // C++ destructors after the bounded drain would create a
            // use-after-free; let the OS reclaim process resources instead.
            std::_Exit(EXIT_FAILURE);
        }
        // 4. Stop postprocess pool (drain all post-process tasks so
        //    free_outputs/release_model complete and model refs reach zero)
        postprocess_pool.stop();
        // 5. FD receiver + event bus
        fd_receiver.stop_all();
        event_bus.disconnect();
        // model_mgr destructor handles HAL deinit
        // hal_loader destructor handles dlclose
        ::unlink(listen_addr.c_str());
        return exit_code;
    };

    if (cfg.auto_infer_enabled && !auto_infer.start()) {
        LOG_FATAL("AutoInfer startup failed; shutting down ai-runtime");
        return shutdown_runtime(EXIT_FAILURE);
    }

    // ── Signal handling ──────────────────────────────────────────────────────
    struct sigaction sa{};
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // ── Wait for shutdown in a separate thread ───────────────────────────────
    // This avoids the deadlock that occurs when calling Shutdown() from signal
    // handler while Wait() holds the internal mutex.
    server_thread = std::thread([&server]() {
        server->Wait();
    });

    // ── Main loop: poll for shutdown signal ──────────────────────────────────
    while (!g_shutdown) {
        // A started pipeline that exits unexpectedly makes the whole runtime
        // unusable; shut down in the same controlled order as a signal.
        if (cfg.auto_infer_enabled && auto_infer.failed()) {
            LOG_FATAL("AutoInfer pipeline exited unexpectedly; "
                      "shutting down ai-runtime");
            return shutdown_runtime(EXIT_FAILURE);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    // ── Graceful shutdown ────────────────────────────────────────────────────
    LOG_INFO("Received shutdown signal, stopping server...");
    shutdown_runtime(0);
    LOG_INFO("Shutdown complete");
    return 0;
}
