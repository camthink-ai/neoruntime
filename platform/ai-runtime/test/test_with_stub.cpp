/**
 * Integration test for ai-runtime C++ using HAL ML stub.
 *
 * Tests all core components without gRPC:
 *   1. HalMlLoader + stub library
 *   2. ModelManager (register/unregister/infer)
 *   3. SessionManager (create/fps/qps)
 *   4. InferenceScheduler (async submit)
 *
 * Build:
 *   g++ -std=c++17 -I../include -I../../../hal_v2/include \
 *       -o test_with_stub test_with_stub.cpp \
 *       ../src/hal_ml_loader.cpp ../src/model_manager.cpp \
 *       ../src/session_manager.cpp ../src/inference_scheduler.cpp \
 *       ../src/config.cpp \
 *       -ldl -lpthread
 *
 * Run:
 *   HAL_STUB_LIB=<path-to-libhal.so> ./test_with_stub
 */

#include "hal_ml_loader.h"
#include "model_manager.h"
#include "session_manager.h"
#include "inference_scheduler.h"
#include "postprocess_pool.h"
#include "config.h"
#include "log.h"
#include "fd_receiver.h"
#include "event_bus_client.h"
#include "auto_infer.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <fstream>
#include <unistd.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <stdexcept>
#include <deque>

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <poll.h>
#include <cerrno>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

using namespace aipc::ai_runtime;
using namespace std::chrono_literals;

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) \
    do { std::cerr << "TEST: " << #name << " ... "; } while(0)

#define PASS() \
    do { std::cerr << "PASS\n"; tests_passed++; } while(0)

#define FAIL(msg) \
    do { std::cerr << "FAIL: " << msg << "\n"; tests_failed++; } while(0)

#define ASSERT_TRUE(expr, msg) \
    do { if (!(expr)) { FAIL(msg); return; } } while(0)

#define ASSERT_EQ(a, b, msg) \
    do { if ((a) != (b)) { FAIL(msg); return; } } while(0)

#define ASSERT_FALSE(expr, msg) ASSERT_TRUE(!(expr), msg)

static std::string get_stub_path() {
    const char* env = std::getenv("HAL_STUB_LIB");
    if (env) return env;

    // Try common build paths
    const char* paths[] = {
        "../../hal_v2/build-stub/libaipc_hal.so",
        "../../../hal_v2/build-stub/libaipc_hal.so",
        "build/output/hal/stub/libaipc_hal.so",
        "../../../build/output/hal/stub/libaipc_hal.so",
        "/opt/aipc/lib/hal/libaipc_hal.so",
    };
    for (auto* p : paths) {
        if (access(p, R_OK) == 0) return p;
    }
    return "";
}

// ─── Mock inference ops (heap-allocating) for alias-refcount test ───────────
// The shipped stub HAL keeps a static singleton session + no-op destroy, so it
// cannot reproduce the use-after-free / double-free that the real (HailoRT) HAL
// exposes when two model aliases share a heap session. This mock models real
// allocation: create() heap-allocates, destroy() heap-frees, so ASan catches a
// double-free or use-after-free.
namespace {
struct MockSession { uint64_t cookie; };

static int g_mock_creates  = 0;
static int g_mock_destroys = 0;

static HalInferenceSession* mock_infer_create(const HalInferenceConfig*) {
    ++g_mock_creates;
    auto* s = new MockSession{0xC0DEFACEULL};
    return reinterpret_cast<HalInferenceSession*>(s);
}
static void mock_infer_destroy(HalInferenceSession* s) {
    if (!s) return;
    ++g_mock_destroys;
    auto* m = reinterpret_cast<MockSession*>(s);
    (void)m->cookie;  // touch -> ASan flags use-after-free on an already-freed ptr
    delete m;
}
static int mock_infer_get_info(HalInferenceSession*, HalModelInfo* info) {
    if (info) { info->num_inputs = 1; info->num_outputs = 1; }
    return 0;
}
static int mock_infer_run(HalInferenceSession*, const HalTensor*, int,
                          HalTensor*, int) { return 0; }

static HalInferenceOps make_mock_infer_ops() {
    HalInferenceOps ops{};
    ops.create         = mock_infer_create;
    ops.destroy        = mock_infer_destroy;
    ops.get_model_info = mock_infer_get_info;
    ops.run            = mock_infer_run;
    return ops;
}

struct AsyncResourceProbe {
    explicit AsyncResourceProbe(std::atomic<bool>* released)
        : released(released) {}
    ~AsyncResourceProbe() { released->store(true); }
    std::atomic<bool>* released;
};

struct BlockingResourceProbe {
    BlockingResourceProbe(std::mutex* mu_in, std::condition_variable* cv_in,
                          bool* destructor_started_in,
                          bool* destructor_finished_in, bool* release_in)
        : mu(mu_in), cv(cv_in), destructor_started(destructor_started_in),
          destructor_finished(destructor_finished_in), release(release_in) {}

    ~BlockingResourceProbe() {
        std::unique_lock lock(*mu);
        *destructor_started = true;
        cv->notify_all();
        cv->wait(lock, [&] { return *release; });
        *destructor_finished = true;
        cv->notify_all();
    }

    std::mutex* mu;
    std::condition_variable* cv;
    bool* destructor_started;
    bool* destructor_finished;
    bool* release;
};

struct AsyncRollbackProbe {
    bool throw_after_allocate = false;
    bool invoke_callback = false;
    bool throw_after_callback = false;
    int async_result = HAL_ERROR;
    int callback_status = HAL_OK;
    int async_calls = 0;
    int sync_calls = 0;
    int allocations = 0;
    int free_calls = 0;
    void* allocated = nullptr;
    const void* expected_input = nullptr;
    uint32_t expected_input_size = 0;
    bool sync_saw_original_input = false;
    bool freed_expected_pointer = false;
    std::atomic<int>* application_completions = nullptr;
    std::atomic<bool>* resource_released = nullptr;
    bool callback_returned_before_application_completion = false;
    bool callback_returned_before_resource_release = false;
    bool callback_returned_before_output_free = false;
};

static AsyncRollbackProbe* g_async_rollback_probe = nullptr;

static int rollback_probe_run(HalInferenceSession*, const HalTensor* inputs,
                              int num_inputs, HalTensor*, int) {
    auto& probe = *g_async_rollback_probe;
    ++probe.sync_calls;
    probe.sync_saw_original_input =
        num_inputs == 1 && inputs[0].data == probe.expected_input &&
        inputs[0].byte_size == probe.expected_input_size &&
        inputs[0].dtype == HAL_DTYPE_UINT8;
    return HAL_OK;
}

static int rollback_probe_run_async(
    HalInferenceSession*, const HalTensor*, int, HalTensor* outputs,
    int num_outputs, HalInferenceAsyncCallback callback, void* userdata) {
    auto& probe = *g_async_rollback_probe;
    ++probe.async_calls;
    assert(num_outputs >= 1);
    assert(callback != nullptr);
    assert(userdata != nullptr);

    auto* buffer = new uint8_t[32];
    outputs[0].data = buffer;
    outputs[0].byte_size = 32;
    probe.allocated = buffer;
    ++probe.allocations;

    if (probe.throw_after_allocate)
        throw std::runtime_error("expected async submission failure");
    if (probe.invoke_callback) {
        callback(outputs, num_outputs, probe.callback_status, userdata);
        probe.callback_returned_before_application_completion =
            probe.application_completions->load() == 0;
        probe.callback_returned_before_resource_release =
            !probe.resource_released->load();
        probe.callback_returned_before_output_free =
            probe.free_calls == 0;
    }
    if (probe.throw_after_callback)
        throw std::runtime_error("expected post-callback submission failure");
    return probe.async_result;
}

static void rollback_probe_free_tensor(HalTensor* tensor) {
    auto& probe = *g_async_rollback_probe;
    ++probe.free_calls;
    probe.freed_expected_pointer = tensor->data == probe.allocated;
    delete[] static_cast<uint8_t*>(tensor->data);
    tensor->data = nullptr;
    tensor->byte_size = 0;
}

static HalInferenceOps make_async_rollback_ops() {
    HalInferenceOps ops = make_mock_infer_ops();
    ops.run = rollback_probe_run;
    ops.run_async = rollback_probe_run_async;
    ops.free_tensor = rollback_probe_free_tensor;
    return ops;
}
}  // namespace

// ─── Test: HAL ML Loader ─────────────────────────────────────────────────────

void test_hal_loader() {
    TEST(hal_loader);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found (set HAL_STUB_LIB)");

    HalMlLoader loader;
    bool ok = loader.load(path);
    ASSERT_TRUE(ok, "dlopen failed");

    ASSERT_TRUE(loader.infer_ops() != nullptr, "infer_ops is null");
    ASSERT_TRUE(loader.infer_ops()->create != nullptr, "infer_ops->create is null");
    ASSERT_TRUE(loader.infer_ops()->run != nullptr, "infer_ops->run is null");

    PASS();
}

// ─── Test: ModelManager ──────────────────────────────────────────────────────

void test_model_manager() {
    TEST(model_manager);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager mgr(loader.infer_ops(), loader.post_ops(), loader.draw_ops(), &loader);

    // Register with the full identity used by the gRPC path.
    const std::string variant =
        R"({"backend_function":"hailo_yolov8n","detection_threshold":0.25})";
    int rc = mgr.register_model("yolo_test", "/fake/model.hef", "app-a",
                                true, variant, "detection");
    ASSERT_EQ(rc, 0, "register_model failed");

    // Same id/path/config from another owner is legitimate co-ownership.
    rc = mgr.register_model("yolo_test", "/fake/model.hef", "app-b",
                            true, variant, "detection");
    ASSERT_EQ(rc, 1, "same-config co-ownership should return existing-entry status");

    // Same id/path but a different decoder identity must collide: accepting
    // either request would let the gRPC layer rewire the incumbent's shared
    // postprocess session after register_model returns.
    std::string why;
    rc = mgr.register_model("yolo_test", "/fake/model.hef", "app-c",
                            true, variant, "classification", &why);
    ASSERT_TRUE(rc != 0, "different model_type should be refused");
    ASSERT_TRUE(why.find("different configuration") != std::string::npos,
                "type collision should carry a useful reason");

    why.clear();
    rc = mgr.register_model(
        "yolo_test", "/fake/model.hef", "app-d", true,
        R"({"backend_function":"hailo_yolov8s","detection_threshold":0.25})",
        "detection", &why);
    ASSERT_TRUE(rc != 0, "different variant should be refused");
    ASSERT_TRUE(why.find("different configuration") != std::string::npos,
                "variant collision should carry a useful reason");

    // Get model via snapshot (rehash-safe)
    auto snap = mgr.acquire_model_snapshot("yolo_test");
    ASSERT_TRUE(snap.has_value(), "acquire_model_snapshot returned nullopt");
    ASSERT_TRUE(snap->infer_session != nullptr, "infer_session should not be null");
    ASSERT_EQ(snap->model_info.num_inputs, 1u, "stub should have 1 input");
    ASSERT_EQ(snap->model_info.num_outputs, 1u, "stub should have 1 output");
    mgr.release_model("yolo_test");

    // List
    auto models = mgr.list_models();
    ASSERT_EQ(models.size(), 1u, "should have 1 model");

    // Infer (sync, with stub)
    HalTensor input{};
    uint8_t dummy_input[640 * 640 * 3] = {};
    input.data = dummy_input;
    input.byte_size = sizeof(dummy_input);
    input.ndim = 4;
    input.shape[0] = 1; input.shape[1] = 3; input.shape[2] = 640; input.shape[3] = 640;
    input.dtype = HAL_DTYPE_UINT8;
    input.dma_fd = -1;

    HalTensor output{};
    uint8_t dummy_output[1024] = {};
    output.data = dummy_output;
    output.byte_size = sizeof(dummy_output);

    rc = mgr.infer(snap->infer_session, &input, 1, &output, 1);
    ASSERT_EQ(rc, 0, "infer should succeed with stub");

    // Refcount: acquire/release
    auto snap2 = mgr.acquire_model_snapshot("yolo_test");
    ASSERT_TRUE(snap2.has_value(), "acquire returned nullopt");

    // Unregister while in use should fail
    rc = mgr.unregister_model("yolo_test");
    ASSERT_TRUE(rc != 0, "unregister with refcount>0 should fail");

    mgr.release_model("yolo_test");

    // Now unregister should succeed
    rc = mgr.unregister_model("yolo_test");
    ASSERT_EQ(rc, 0, "unregister after release should succeed");

    ASSERT_TRUE(!mgr.acquire_model_snapshot("yolo_test").has_value(), "model should be gone");

    PASS();
}

// ─── Test: owner-scoped unregister is transactional ─────────────────────────
void test_owner_scoped_unregister() {
    TEST(owner_scoped_unregister);

    g_mock_creates  = 0;
    g_mock_destroys = 0;
    HalInferenceOps infer_ops = make_mock_infer_ops();
    ModelManager mgr(&infer_ops, nullptr, nullptr, nullptr);

    int rc = mgr.register_model("owned", "/fake/owned.hef", "app-a");
    ASSERT_EQ(rc, 0, "initial owner registration failed");
    rc = mgr.register_model("owned", "/fake/owned.hef", "app-b");
    ASSERT_EQ(rc, 1, "second owner should co-own existing entry");

    // A foreign/duplicate scoped release is a no-op, never a force unload.
    rc = mgr.unregister_model("owned", "not-an-owner");
    ASSERT_EQ(rc, 1, "absent owner release should keep the physical model");
    ASSERT_TRUE(mgr.is_owner("owned", "app-a"), "app-a ownership was disturbed");
    ASSERT_TRUE(mgr.is_owner("owned", "app-b"), "app-b ownership was disturbed");
    ASSERT_EQ(g_mock_destroys, 0, "absent owner must not destroy the model");

    // Releasing one of two owners keeps the shared registration resident.
    rc = mgr.unregister_model("owned", "app-a");
    ASSERT_EQ(rc, 1, "co-owner release should keep the physical model");
    ASSERT_TRUE(!mgr.is_owner("owned", "app-a"), "app-a should be released");
    ASSERT_TRUE(mgr.is_owner("owned", "app-b"), "app-b must remain");
    ASSERT_EQ(g_mock_destroys, 0, "co-owner release must not destroy the model");

    // Busy last-owner release is refused without deleting that ownership.
    auto snap = mgr.acquire_model_snapshot("owned");
    ASSERT_TRUE(snap.has_value(), "owned model missing");
    rc = mgr.unregister_model("owned", "app-b");
    ASSERT_TRUE(rc != 0, "busy last-owner release should fail");
    ASSERT_TRUE(mgr.is_owner("owned", "app-b"),
                "busy refusal must retain the last owner");
    mgr.release_model("owned");

    // Once idle, the last owner and physical session disappear together.
    rc = mgr.unregister_model("owned", "app-b");
    ASSERT_EQ(rc, 0, "idle last-owner release failed");
    ASSERT_TRUE(!mgr.acquire_model_snapshot("owned").has_value(),
                "last-owner release must remove the model");
    ASSERT_EQ(g_mock_destroys, 1, "physical session should be destroyed once");

    PASS();
}

void test_force_unregister_all_is_atomic_when_busy() {
    TEST(force_unregister_all_is_atomic_when_busy);

    g_mock_creates = 0;
    g_mock_destroys = 0;
    HalInferenceOps infer_ops = make_mock_infer_ops();
    ModelManager mgr(&infer_ops, nullptr, nullptr, nullptr);

    ASSERT_EQ(mgr.register_model("busy", "/fake/busy.hef", "app-a"), 0,
              "busy model registration failed");
    ASSERT_EQ(mgr.register_model("idle", "/fake/idle.hef", "app-b"), 0,
              "idle model registration failed");
    auto busy = mgr.acquire_model_snapshot("busy");
    ASSERT_TRUE(busy.has_value(), "busy model acquisition failed");

    ASSERT_TRUE(!mgr.force_unregister_all(),
                "bulk unload should refuse a live model reference");
    ASSERT_EQ(mgr.list_models().size(), 2u,
              "busy refusal must leave the full registry untouched");
    ASSERT_TRUE(mgr.is_owner("busy", "app-a"),
                "busy refusal removed model ownership");
    ASSERT_TRUE(mgr.is_owner("idle", "app-b"),
                "busy refusal partially removed another model");
    ASSERT_EQ(g_mock_destroys, 0,
              "busy refusal must not destroy any HAL session");

    mgr.release_model("busy");
    ASSERT_TRUE(mgr.force_unregister_all(),
                "idle bulk unload should succeed");
    ASSERT_TRUE(mgr.list_models().empty(),
                "successful bulk unload left models registered");
    ASSERT_EQ(g_mock_destroys, 2,
              "successful bulk unload should destroy both sessions once");

    PASS();
}

// ─── Test: model alias session refcount (P0 UAF / double-free) ───────────────
// Validates the ModelManager session-refcount fix for path aliases: two model
// ids on the same file share one heap infer_session; it must be destroyed
// exactly once. Under the old code ASan aborts with double-free here.
void test_model_alias_refcount() {
    TEST(model_alias_refcount);

    g_mock_creates  = 0;
    g_mock_destroys = 0;
    HalInferenceOps infer_ops = make_mock_infer_ops();

    // Part 1: explicit unregister of both aliases (the alias UAF / double-free path).
    {
        ModelManager mgr(&infer_ops, nullptr, nullptr, nullptr);

        int rc = mgr.register_model("alias_a", "/fake/model.hef");
        ASSERT_EQ(rc, 0, "register alias_a failed");

        rc = mgr.register_model("alias_b", "/fake/model.hef");  // same path -> alias
        ASSERT_EQ(rc, 0, "register alias_b failed");

        ASSERT_EQ(g_mock_creates, 1, "alias must NOT create a second session");

        // Both aliases must resolve to the SAME heap session pointer.
        auto snap_a = mgr.acquire_model_snapshot("alias_a");
        auto snap_b = mgr.acquire_model_snapshot("alias_b");
        ASSERT_TRUE(snap_a.has_value() && snap_b.has_value(), "snapshots missing");
        ASSERT_TRUE(snap_a->infer_session != nullptr, "alias_a session null");
        ASSERT_EQ(snap_a->infer_session, snap_b->infer_session,
                  "aliases must share one infer_session");
        HalInferenceSession* shared = snap_a->infer_session;
        mgr.release_model("alias_a");
        mgr.release_model("alias_b");

        // Unregister one alias: shared session must survive (refcount 2 -> 1).
        rc = mgr.unregister_model("alias_a");
        ASSERT_EQ(rc, 0, "unregister alias_a failed");
        ASSERT_EQ(g_mock_destroys, 0, "shared session must NOT be freed while an alias lives");

        // Surviving alias still holds the same, valid pointer.
        auto snap_b2 = mgr.acquire_model_snapshot("alias_b");
        ASSERT_TRUE(snap_b2.has_value(), "alias_b should still exist");
        ASSERT_EQ(snap_b2->infer_session, shared, "alias_b session was invalidated");
        mgr.release_model("alias_b");

        // Last alias gone: physical session destroyed exactly once.
        rc = mgr.unregister_model("alias_b");
        ASSERT_EQ(rc, 0, "unregister alias_b failed");
        ASSERT_EQ(g_mock_destroys, 1, "session must be freed exactly once");
    }
    ASSERT_EQ(g_mock_creates, g_mock_destroys, "create/destroy must balance");

    // Part 2: destructor cleanup of shared sessions (no explicit unregister) —
    // ~ModelManager() must destroy the shared session once, not once per alias.
    g_mock_creates  = 0;
    g_mock_destroys = 0;
    {
        ModelManager mgr(&infer_ops, nullptr, nullptr, nullptr);
        int rc = mgr.register_model("alias_c", "/fake/model.hef");
        ASSERT_EQ(rc, 0, "register alias_c failed");
        rc = mgr.register_model("alias_d", "/fake/model.hef");
        ASSERT_EQ(rc, 0, "register alias_d failed");
        rc = mgr.register_model("alias_e", "/fake/model.hef");
        ASSERT_EQ(rc, 0, "register alias_e failed");
        ASSERT_EQ(g_mock_creates, 1, "three aliases -> one shared session");
    }
    ASSERT_EQ(g_mock_destroys, 1, "destructor must free the shared session exactly once");
    ASSERT_EQ(g_mock_creates, g_mock_destroys, "destructor: create/destroy must balance");

    PASS();
}

// ─── Test: StreamInfer config ────────────────────────────────────────────────

void test_stream_infer_config() {
    TEST(stream_infer_config);

    const std::string missing_path =
        "/tmp/ai-runtime-config-missing-" + std::to_string(getpid());
    unlink(missing_path.c_str());
    const auto defaults = load_config(missing_path);
    ASSERT_EQ(defaults.stream_max_active_rpcs, 16u,
              "default max_active_rpcs mismatch");
    ASSERT_EQ(defaults.stream_max_active_rpcs_per_peer, 16u,
              "default max_active_rpcs_per_peer mismatch");
    ASSERT_EQ(defaults.stream_max_subscribers_per_stream, 4u,
              "default max_subscribers_per_stream mismatch");
    ASSERT_EQ(defaults.stream_max_in_flight_total, 12u,
              "default max_in_flight_total mismatch");
    ASSERT_EQ(defaults.stream_max_in_flight_per_rpc, 3u,
              "default max_in_flight_per_rpc mismatch");

    const std::string path =
        "/tmp/ai-runtime-config-test-" + std::to_string(getpid()) + ".yaml";
    {
        std::ofstream out(path, std::ios::trunc);
        ASSERT_TRUE(out.good(), "failed to create config test file");
        out << "stream_infer:\n"
            << "  max_active_rpcs: 21\n"
            << "  max_active_rpcs_per_peer: 22\n"
            << "  max_subscribers_per_stream: 23\n"
            << "  max_in_flight_total: 24\n"
            << "  max_in_flight_per_rpc: 25\n";
    }
    const auto parsed = load_config(path);
    ASSERT_EQ(parsed.stream_max_active_rpcs, 21u,
              "configured max_active_rpcs mismatch");
    ASSERT_EQ(parsed.stream_max_active_rpcs_per_peer, 22u,
              "configured max_active_rpcs_per_peer mismatch");
    ASSERT_EQ(parsed.stream_max_subscribers_per_stream, 23u,
              "configured max_subscribers_per_stream mismatch");
    ASSERT_EQ(parsed.stream_max_in_flight_total, 24u,
              "configured max_in_flight_total mismatch");
    ASSERT_EQ(parsed.stream_max_in_flight_per_rpc, 25u,
              "configured max_in_flight_per_rpc mismatch");

    {
        std::ofstream out(path, std::ios::trunc);
        out << "stream_infer:\n"
            << "  max_active_rpcs: 0\n"
            << "  max_active_rpcs_per_peer: 0\n"
            << "  max_subscribers_per_stream: 0\n"
            << "  max_in_flight_total: 0\n"
            << "  max_in_flight_per_rpc: 0\n";
    }
    const auto unlimited = load_config(path);
    ASSERT_EQ(unlimited.stream_max_active_rpcs, 0u,
              "zero max_active_rpcs should be preserved");
    ASSERT_EQ(unlimited.stream_max_active_rpcs_per_peer, 0u,
              "zero per-peer limit should be preserved");
    ASSERT_EQ(unlimited.stream_max_subscribers_per_stream, 0u,
              "zero per-stream limit should be preserved");
    ASSERT_EQ(unlimited.stream_max_in_flight_total, 0u,
              "zero aggregate work limit should be preserved");
    ASSERT_EQ(unlimited.stream_max_in_flight_per_rpc, 0u,
              "zero per-rpc work limit should be preserved");

    for (const char* invalid : {"not-a-number", "4294967296", "-1"}) {
        {
            std::ofstream out(path, std::ios::trunc);
            out << "stream_infer:\n  max_active_rpcs: " << invalid << "\n";
        }
        bool threw = false;
        try {
            (void)load_config(path);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        ASSERT_TRUE(threw, "invalid stream admission value was accepted");
    }

    unlink(path.c_str());
    PASS();
}

// ─── Test: SessionManager ────────────────────────────────────────────────────

void test_session_manager() {
    TEST(session_manager);

    SessionManager mgr;

    auto sid = mgr.create_session("app1", "cam0_main", "yolo", 30, 100, 5);
    ASSERT_TRUE(!sid.empty(), "session_id should not be empty");

    // get_session now returns shared_ptr<Session>; hold it so the session stays
    // alive for the duration of these checks.
    auto s = mgr.get_session(sid);
    ASSERT_TRUE(s != nullptr, "session not found");
    ASSERT_EQ(s->fps_limit, 30u, "fps_limit mismatch");
    ASSERT_EQ(s->model_id, std::string("yolo"), "model_id mismatch");

    // FPS limit: first call should pass, immediate second should fail.
    // check_*/record_inference take Session* — pass the raw ptr from the
    // shared_ptr we hold (lifetime guaranteed by `s` above).
    bool ok = mgr.check_fps_limit(s.get());
    ASSERT_TRUE(ok, "first fps check should pass");
    mgr.record_inference(s.get());
    ok = mgr.check_fps_limit(s.get());
    ASSERT_TRUE(!ok, "immediate second fps check at 30fps should fail");

    // QPS limit
    ok = mgr.check_qps_limit(s.get());
    ASSERT_TRUE(ok, "qps check should pass (1 infer << 100 qps)");

    // List
    auto sessions = mgr.list_sessions();
    ASSERT_EQ(sessions.size(), 1u, "should have 1 session");

    // Destroy
    bool destroyed = mgr.destroy_session(sid);
    ASSERT_TRUE(destroyed, "destroy should succeed");
    ASSERT_TRUE(mgr.get_session(sid) == nullptr, "session should be gone");

    PASS();
}

// ─── Test: SessionManager concurrent Infer + UnregisterModel (UAF repro) ─────
//
// Reproduces the grpcpp_sync_ser crash root cause: the gRPC sync server runs a
// worker pool, so concurrent requests race. Commit 6c7df60 added
// UnregisterModel -> destroy_sessions_by_model (which erases the implicit
// session) while a single-shot Infer held a raw Session* across the FULL
// inference then called record_inference. Concurrent unregister + Infer freed
// the session mid-use -> heap corruption -> "corrupted double-linked list"
// SIGABRT.
//
// This test stresses exactly that window at the SessionManager API level:
//   * "infer" workers  : create_named_session -> get_session -> hold across a
//                        work window -> record_inference
//   * "destroy" workers: destroy_sessions_by_model in a tight loop
//
// With the shared_ptr ownership fix, get_session returns a shared_ptr whose
// copy keeps the session alive until record_inference returns, so there is no
// use-after-free. Under -DENABLE_ASAN=ON this must report NO error. (With the
// pre-fix raw-pointer API the equivalent code triggers a heap-use-after-free
// here, which is the whole point of the harness.)
void test_session_ids_are_unique_under_concurrency() {
    TEST(session_ids_are_unique_under_concurrency);

    SessionManager mgr;
    constexpr int kThreads = 8;
    // Per-thread app keeps every create inside the per-app cap (128) and the
    // total inside the global cap (1024): this test is about id uniqueness
    // under concurrency, not about the creation caps (covered separately by
    // test_session_creation_policy below).
    constexpr int kSessionsPerThread = 125;
    std::atomic<bool> saw_empty_id{false};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int thread_index = 0; thread_index < kThreads; ++thread_index) {
        threads.emplace_back([&, thread_index] {
            const std::string app = "app-" + std::to_string(thread_index);
            for (int i = 0; i < kSessionsPerThread; ++i) {
                const auto id = mgr.create_session(
                    app, "same-stream", "same-model", 0, 0, 5);
                if (id.empty())
                    saw_empty_id.store(true, std::memory_order_relaxed);
            }
        });
    }
    for (auto& thread : threads)
        thread.join();

    ASSERT_TRUE(!saw_empty_id.load(std::memory_order_relaxed),
                "concurrent session creation returned an empty id");
    ASSERT_EQ(mgr.list_sessions().size(),
              static_cast<size_t>(kThreads * kSessionsPerThread),
              "concurrent session ids must be unique");

    PASS();
}

// SessionManager creation policy: identifier whitelist, per-app cap, global
// cap, and named-session conflict rejection. Every rejection must return ""
// and leave the registry untouched.
void test_session_creation_policy() {
    TEST(session_creation_policy);

    {
        SessionManager mgr;

        // Charset + length whitelist on every identifier component.
        ASSERT_TRUE(mgr.create_session("bad app", "s", "m", 0, 0, 5).empty(),
                    "space in app_id must be rejected");
        ASSERT_TRUE(mgr.create_session("app", "s", "m\x01", 0, 0, 5).empty(),
                    "control byte in model_id must be rejected");
        ASSERT_TRUE(
            mgr.create_session("a/b", "s", "m", 0, 0, 5).empty(),
            "path separator in app_id must be rejected");
        const std::string long_id(SessionManager::kMaxIdentifierBytes + 1, 'a');
        ASSERT_TRUE(mgr.create_session(long_id, "s", "m", 0, 0, 5).empty(),
                    "over-length identifier must be rejected");
        ASSERT_EQ(mgr.list_sessions().size(), 0u,
                  "rejected creates must not add sessions");
    }

    {
        SessionManager mgr;
        // Named sessions: exact-tuple idempotency, anything else rejected.
        const std::string sid = "named-1";
        ASSERT_EQ(mgr.create_named_session(sid, "app", "cam", "model", 30, 5, 4),
                  sid, "first named create must succeed");
        ASSERT_EQ(
            mgr.create_named_session(sid, "app", "cam", "model", 30, 5, 4),
            sid, "same identity/config tuple must be idempotent");
        ASSERT_TRUE(
            mgr.create_named_session(sid, "app", "cam", "model", 15, 5, 4)
                .empty(),
            "named reuse with different fps_limit must be rejected");
        ASSERT_TRUE(
            mgr.create_named_session(sid, "other-app", "cam", "model", 30, 5, 4)
                .empty(),
            "named reuse with different app_id must be rejected");
        ASSERT_TRUE(
            mgr.create_named_session("", "app", "cam", "model", 30, 5, 4)
                .empty(),
            "empty named id must be rejected");
        ASSERT_EQ(mgr.list_sessions().size(), 1u,
                  "rejected named creates must not add sessions");
    }

    {
        // Per-app cap: the (kMaxSessionsPerApp)-th create is the last that
        // succeeds; another app is unaffected.
        SessionManager mgr;
        for (size_t i = 0; i < SessionManager::kMaxSessionsPerApp; i++) {
            ASSERT_FALSE(mgr.create_session("capapp", "s", "m", 0, 0, 5).empty(),
                         "creates below the per-app cap must succeed");
        }
        ASSERT_TRUE(mgr.create_session("capapp", "s", "m", 0, 0, 5).empty(),
                    "per-app cap must reject further creates");
        ASSERT_FALSE(mgr.create_session("other-app", "s", "m", 0, 0, 5).empty(),
                     "a different app must not share the cap");
    }

    {
        // Global cap: total sessions are bounded regardless of app spread.
        SessionManager mgr;
        size_t created = 0;
        for (size_t i = 0; i < SessionManager::kMaxSessionsTotal + 10; i++) {
            // Spread over 100-session apps so the per-app cap never fires
            // first; the global cap is the only bound that can trip.
            const std::string app = "gapp-" + std::to_string(i / 100);
            if (!mgr.create_session(app, "s", "m", 0, 0, 5).empty())
                created++;
        }
        ASSERT_EQ(created, SessionManager::kMaxSessionsTotal,
                  "global cap must bound total sessions");
    }

    {
        // Squat: a named session can pre-register an id that looks like a
        // future generated one. create_session must not hand out the
        // squatted id (it belongs to another config); it regenerates.
        SessionManager mgr;
        const std::string first = mgr.create_session("app", "s", "m", 0, 0, 5);
        ASSERT_FALSE(first.empty(), "seed create must succeed");
        const size_t dash = first.rfind('-');
        ASSERT_TRUE(dash != std::string::npos, "generated id must be parseable");
        const uint64_t next_seq =
            std::strtoull(first.substr(dash + 1).c_str(), nullptr, 10) + 1;
        const std::string squatted =
            "app-s-m-" + std::to_string(next_seq);
        ASSERT_FALSE(mgr.create_named_session(squatted, "app", "s", "m",
                                              1, 2, 3)
                         .empty(),
                     "squatting a generated-looking id must itself succeed");
        const std::string generated = mgr.create_session("app", "s", "m", 0, 0, 5);
        ASSERT_FALSE(generated.empty(),
                     "create after a squat must still succeed");
        ASSERT_TRUE(generated != squatted,
                    "create_session must not return the squatted session id");
        auto squatted_session = mgr.get_session(squatted);
        ASSERT_TRUE(squatted_session && squatted_session->fps_limit == 1,
                    "the squatted session must keep its own config");
    }

    PASS();
}

void test_session_concurrent_destroy() {
    TEST(session_concurrent_destroy);

    SessionManager mgr;
    const std::string model = "yolo_conc";
    const std::string sid   = "implicit-yolo_conc";

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> infer_iters{0};

    // "Infer" workers: mimic the Infer() path — look up the implicit session,
    // hold it across a (short) work window, then record_inference. Holding the
    // shared_ptr returned by get_session is what prevents the UAF: even if a
    // destroy worker erases the map entry concurrently, this copy keeps the
    // object alive until record_inference returns and `s` drops.
    auto infer_worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            mgr.create_named_session(sid, "app", "cam0", model, 0, 0, 5);
            auto s = mgr.get_session(sid);
            if (s) {
                // Simulate the inference window during which another thread may
                // UnregisterModel -> destroy_sessions_by_model. A busy spin (no
                // syscall) keeps the window tight and the race rate high.
                for (volatile int i = 0; i < 64; ++i) {}
                // MUST NOT use-after-free: s keeps the session alive.
                mgr.record_inference(s.get(), 100);
                infer_iters.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    // "Unregister" workers: mimic UnregisterModel repeatedly erasing the
    // implicit session while infers are in flight.
    auto destroy_worker = [&]() {
        while (!stop.load(std::memory_order_relaxed)) {
            mgr.destroy_sessions_by_model(model);
        }
    };

    const int N_INFER = 4;
    std::vector<std::thread> threads;
    threads.reserve(N_INFER + 2);
    for (int i = 0; i < N_INFER; ++i) threads.emplace_back(infer_worker);
    threads.emplace_back(destroy_worker);
    threads.emplace_back(destroy_worker);

    // Bounded run. Thousands of iterations across 6 threads give ample chance
    // for the race; ASan checks every access.
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : threads) t.join();

    ASSERT_TRUE(infer_iters.load(std::memory_order_relaxed) > 0,
                "infer workers should have made progress");

    PASS();
}

void test_postprocess_pool_stop_stress() {
    TEST(postprocess_pool_stop_stress);

    for (int iteration = 0; iteration < 100; ++iteration) {
        PostprocessPool pool(1, 8);
        std::atomic<int> completed{0};
        pool.start();
        for (int i = 0; i < 8; ++i) {
            PostprocessPool::Task task = [&completed] {
                completed.fetch_add(1, std::memory_order_relaxed);
            };
            ASSERT_TRUE(pool.submit(task), "postprocess task submission failed");
        }
        pool.stop();
        ASSERT_EQ(completed.load(std::memory_order_relaxed), 8,
                  "postprocess stop did not drain all queued tasks");
    }

    PASS();
}

// ─── Test: InferenceScheduler ────────────────────────────────────────────────

void test_inference_scheduler() {
    TEST(inference_scheduler);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager model_mgr(loader.infer_ops(), loader.post_ops(), loader.draw_ops(), &loader);
    SessionManager session_mgr;

    int rc = model_mgr.register_model("sched_test", "/fake/model.hef");
    ASSERT_EQ(rc, 0, "register failed");

    auto sid = session_mgr.create_session("app", "cam0", "sched_test", 0, 0, 5);

    InferenceScheduler scheduler(&model_mgr, &session_mgr, 2, 16);
    scheduler.start();

    // Submit async request
    std::mutex mu;
    std::condition_variable cv;
    bool done = false;
    int result_rc = -999;
    uint64_t infer_us = 0;

    HalTensor input{};
    uint8_t dummy[1024] = {};
    input.data = dummy;
    input.byte_size = sizeof(dummy);
    input.ndim = 4;
    input.shape[0] = 1; input.shape[1] = 3; input.shape[2] = 640; input.shape[3] = 640;
    input.dtype = HAL_DTYPE_UINT8;
    input.dma_fd = -1;

    auto req = std::make_unique<InferRequest>();
    req->session_id = sid;
    req->model_id   = "sched_test";
    req->inputs[0]   = input;
    req->num_inputs  = 1;
    req->priority    = 5;
    req->timeout_ms  = 5000;
    req->on_complete = [&](int rc_val, HalTensor*, int, uint64_t ius, uint64_t, bool) {
        std::lock_guard lock(mu);
        result_rc = rc_val;
        infer_us  = ius;
        done = true;
        cv.notify_one();
    };

    bool submitted = scheduler.submit(std::move(req));
    ASSERT_TRUE(submitted, "submit should succeed");

    // Wait for result
    {
        std::unique_lock lock(mu);
        bool ok = cv.wait_for(lock, std::chrono::seconds(5), [&] { return done; });
        ASSERT_TRUE(ok, "timed out waiting for inference result");
    }

    ASSERT_EQ(result_rc, 0, "inference should succeed");
    ASSERT_TRUE(infer_us >= 0, "infer_us should be >= 0");  /* stub may return instantly */

    scheduler.stop();

    PASS();
}

namespace {
struct AsyncRollbackResult {
    bool submitted = false;
    bool completed = false;
    int callback_calls = 0;
    int callback_rc = -999;
    bool callback_model_acquired = false;
    int async_calls = 0;
    int sync_calls = 0;
    int allocations = 0;
    int free_calls = 0;
    bool sync_saw_original_input = false;
    bool freed_expected_pointer = false;
    bool callback_returned_before_application_completion = false;
    bool callback_returned_before_resource_release = false;
    bool callback_returned_before_output_free = false;
    int tracker_balance = -999;
    int unregister_rc = -999;
};

AsyncRollbackResult run_async_rollback_case(
    bool throw_after_allocate, bool invoke_callback = false,
    bool throw_after_callback = false, int async_result = HAL_ERROR) {
    AsyncRollbackResult result;
    AsyncRollbackProbe probe;
    probe.throw_after_allocate = throw_after_allocate;
    probe.invoke_callback = invoke_callback;
    probe.throw_after_callback = throw_after_callback;
    probe.async_result = async_result;
    std::atomic<int> application_completions{0};
    std::atomic<bool> resource_released{false};
    probe.application_completions = &application_completions;
    probe.resource_released = &resource_released;
    g_async_rollback_probe = &probe;

    HalInferenceOps infer_ops = make_async_rollback_ops();
    ModelManager model_mgr(&infer_ops, nullptr, nullptr, nullptr);
    SessionManager session_mgr;
    assert(model_mgr.register_model("async_rollback", "/fake/model.hef") == 0);
    auto sid = session_mgr.create_session(
        "app", "cam0", "async_rollback", 0, 0, 5);

    InferenceScheduler scheduler(&model_mgr, &session_mgr, 1, 4);
    scheduler.start();

    uint8_t input_buffer[37] = {};
    probe.expected_input = input_buffer;
    probe.expected_input_size = sizeof(input_buffer);

    std::mutex mu;
    std::condition_variable cv;
    auto req = std::make_unique<InferRequest>();
    req->session_id = sid;
    req->model_id = "async_rollback";
    req->inputs[0].data = input_buffer;
    req->inputs[0].byte_size = sizeof(input_buffer);
    req->inputs[0].dtype = HAL_DTYPE_UINT8;
    req->inputs[0].dma_fd = -1;
    req->num_inputs = 1;
    req->resource_holder =
        std::make_shared<AsyncResourceProbe>(&resource_released);
    req->on_complete = [&](int rc, HalTensor*, int, uint64_t, uint64_t,
                           bool model_acquired) {
        application_completions.fetch_add(1);
        std::lock_guard lock(mu);
        ++result.callback_calls;
        result.callback_rc = rc;
        result.callback_model_acquired = model_acquired;
        result.completed = true;
        cv.notify_all();
    };

    result.submitted = scheduler.submit(std::move(req));
    if (result.submitted) {
        std::unique_lock lock(mu);
        cv.wait_for(lock, std::chrono::seconds(2), [&] {
            return result.completed;
        });
    }
    scheduler.stop();

    result.async_calls = probe.async_calls;
    result.sync_calls = probe.sync_calls;
    result.allocations = probe.allocations;
    result.free_calls = probe.free_calls;
    result.sync_saw_original_input = probe.sync_saw_original_input;
    result.freed_expected_pointer = probe.freed_expected_pointer;
    result.callback_returned_before_application_completion =
        probe.callback_returned_before_application_completion;
    result.callback_returned_before_resource_release =
        probe.callback_returned_before_resource_release;
    result.callback_returned_before_output_free =
        probe.callback_returned_before_output_free;
    result.tracker_balance = scheduler.drain_async(0);
    result.unregister_rc = model_mgr.unregister_model("async_rollback");
    g_async_rollback_probe = nullptr;
    return result;
}

void assert_async_rollback_result(const AsyncRollbackResult& result) {
    ASSERT_TRUE(result.submitted, "scheduler rejected rollback test request");
    ASSERT_TRUE(result.completed, "sync fallback did not complete");
    ASSERT_EQ(result.callback_calls, 1, "completion callback count mismatch");
    ASSERT_EQ(result.callback_rc, HAL_OK, "sync fallback should succeed");
    ASSERT_TRUE(result.callback_model_acquired,
                "callback should observe the worker model reference");
    ASSERT_EQ(result.async_calls, 1, "async submit count mismatch");
    ASSERT_EQ(result.sync_calls, 1, "sync fallback count mismatch");
    ASSERT_EQ(result.allocations, 1, "failed async allocation count mismatch");
    ASSERT_EQ(result.free_calls, 1, "failed async output was not freed once");
    ASSERT_TRUE(result.sync_saw_original_input,
                "sync fallback did not receive the original input");
    ASSERT_TRUE(result.freed_expected_pointer,
                "cleanup freed an unexpected output pointer");
    ASSERT_EQ(result.tracker_balance, 0, "async tracker was not balanced");
    ASSERT_EQ(result.unregister_rc, 0, "worker model reference leaked");
}
}  // namespace

void test_inference_scheduler_async_reject_rolls_back() {
    TEST(inference_scheduler_async_reject_rolls_back);
    const int failures_before = tests_failed;
    const auto result = run_async_rollback_case(false);
    assert_async_rollback_result(result);
    if (tests_failed == failures_before) PASS();
}

void test_inference_scheduler_async_throw_rolls_back() {
    TEST(inference_scheduler_async_throw_rolls_back);
    const int failures_before = tests_failed;
    const auto result = run_async_rollback_case(true);
    assert_async_rollback_result(result);
    if (tests_failed == failures_before) PASS();
}

namespace {
void assert_inline_async_result(const AsyncRollbackResult& result) {
    ASSERT_TRUE(result.submitted, "scheduler rejected inline callback request");
    ASSERT_TRUE(result.completed, "inline async callback did not complete");
    ASSERT_EQ(result.callback_calls, 1, "inline completion count mismatch");
    ASSERT_EQ(result.callback_rc, HAL_OK, "inline callback status mismatch");
    ASSERT_TRUE(result.callback_model_acquired,
                "inline callback did not own the worker model reference");
    ASSERT_EQ(result.async_calls, 1, "inline async submit count mismatch");
    ASSERT_EQ(result.sync_calls, 0,
              "callback-owned submission incorrectly ran sync fallback");
    ASSERT_EQ(result.allocations, 1, "inline allocation count mismatch");
    ASSERT_EQ(result.free_calls, 1, "inline output was not freed once");
    ASSERT_TRUE(result.freed_expected_pointer,
                "inline cleanup freed an unexpected output pointer");
    ASSERT_TRUE(result.callback_returned_before_application_completion,
                "application completion ran inside run_async callback");
    ASSERT_TRUE(result.callback_returned_before_resource_release,
                "input resource holder was released before run_async returned");
    ASSERT_TRUE(result.callback_returned_before_output_free,
                "output backing was freed before run_async returned");
    ASSERT_EQ(result.tracker_balance, 0, "inline async tracker was not balanced");
    ASSERT_EQ(result.unregister_rc, 0, "inline callback leaked a model ref");
}
}  // namespace

void test_inference_scheduler_inline_callback_before_success_return() {
    TEST(inference_scheduler_inline_callback_before_success_return);
    const int failures_before = tests_failed;
    const auto result = run_async_rollback_case(
        false, true, false, HAL_OK);
    assert_inline_async_result(result);
    if (tests_failed == failures_before) PASS();
}

void test_inference_scheduler_inline_callback_then_error() {
    TEST(inference_scheduler_inline_callback_then_error);
    const int failures_before = tests_failed;
    const auto result = run_async_rollback_case(
        false, true, false, HAL_ERROR);
    assert_inline_async_result(result);
    if (tests_failed == failures_before) PASS();
}

void test_inference_scheduler_inline_callback_then_throw() {
    TEST(inference_scheduler_inline_callback_then_throw);
    const int failures_before = tests_failed;
    const auto result = run_async_rollback_case(
        false, true, true, HAL_ERROR);
    assert_inline_async_result(result);
    if (tests_failed == failures_before) PASS();
}

void test_inference_scheduler_submit_stop_race() {
    TEST(inference_scheduler_submit_stop_race);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager model_mgr(loader.infer_ops(), loader.post_ops(),
                           loader.draw_ops(), &loader);
    SessionManager session_mgr;
    InferenceScheduler scheduler(&model_mgr, &session_mgr, 1, 1);
    scheduler.start();

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool at_gate = false;
    bool release_submit = false;
    scheduler.set_submit_prelock_hook_for_test([&] {
        std::unique_lock lock(gate_mu);
        at_gate = true;
        gate_cv.notify_all();
        gate_cv.wait(lock, [&] { return release_submit; });
    });

    std::atomic<bool> submitted{true};
    std::thread submit_thread([&] {
        auto req = std::make_unique<InferRequest>();
        req->session_id = "stopped-session";
        req->model_id = "stopped-model";
        submitted.store(scheduler.submit(std::move(req)),
                        std::memory_order_release);
    });

    {
        std::unique_lock lock(gate_mu);
        bool reached = gate_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return at_gate; });
        if (!reached) {
            release_submit = true;
            gate_cv.notify_all();
            lock.unlock();
            submit_thread.join();
            scheduler.stop();
            FAIL("submit did not reach pre-lock gate");
            return;
        }
    }

    scheduler.stop();
    {
        std::lock_guard lock(gate_mu);
        release_submit = true;
        gate_cv.notify_all();
    }
    submit_thread.join();

    ASSERT_TRUE(!submitted.load(std::memory_order_acquire),
                "submit accepted a request after stop completed");
    ASSERT_EQ(scheduler.queue_depth(), 0,
              "stopped scheduler retained an unserviceable request");

    PASS();
}

void test_inference_scheduler_external_async_drain() {
    TEST(inference_scheduler_external_async_drain);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager model_mgr(loader.infer_ops(), loader.post_ops(),
                           loader.draw_ops(), &loader);
    SessionManager session_mgr;
    InferenceScheduler scheduler(&model_mgr, &session_mgr, 1, 1);

    scheduler.begin_external_async();

    std::mutex gate_mu;
    std::condition_variable gate_cv;
    bool drain_waiting = false;
    scheduler.set_async_drain_wait_hook_for_test([&] {
        std::lock_guard lock(gate_mu);
        drain_waiting = true;
        gate_cv.notify_all();
    });

    std::atomic<bool> drain_done{false};
    int drain_result = -1;
    std::thread drain_thread([&] {
        drain_result = scheduler.drain_async(2000);
        drain_done.store(true, std::memory_order_release);
    });

    {
        std::unique_lock lock(gate_mu);
        const bool entered_wait = gate_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return drain_waiting; });
        if (!entered_wait) {
            scheduler.complete_external_async();
            lock.unlock();
            drain_thread.join();
            FAIL("external drain did not enter its outstanding-work wait");
            return;
        }
    }
    const bool waited_for_completion =
        !drain_done.load(std::memory_order_acquire);

    scheduler.complete_external_async();
    drain_thread.join();

    ASSERT_TRUE(waited_for_completion,
                "drain returned before external async work completed");
    ASSERT_EQ(drain_result, 0,
              "drain reported an orphan after external completion");

    PASS();
}

void test_inference_scheduler_drain_waits_for_resource_cleanup() {
    TEST(inference_scheduler_drain_waits_for_resource_cleanup);

    AsyncRollbackProbe probe;
    probe.invoke_callback = true;
    probe.async_result = HAL_OK;
    probe.callback_status = HAL_OK;
    std::atomic<int> application_completions{0};
    std::atomic<bool> resource_released{false};
    probe.application_completions = &application_completions;
    probe.resource_released = &resource_released;
    g_async_rollback_probe = &probe;

    HalInferenceOps infer_ops = make_async_rollback_ops();
    ModelManager model_mgr(&infer_ops, nullptr, nullptr, nullptr);
    SessionManager session_mgr;
    ASSERT_EQ(model_mgr.register_model("async_cleanup", "/fake/model.hef"),
              0, "register_model failed");
    const auto sid = session_mgr.create_session(
        "app", "cam0", "async_cleanup", 0, 0, 5);

    InferenceScheduler scheduler(&model_mgr, &session_mgr, 1, 4);
    scheduler.start();

    std::mutex completion_mu;
    std::condition_variable completion_cv;
    bool completion_called = false;
    std::mutex resource_mu;
    std::condition_variable resource_cv;
    bool destructor_started = false;
    bool destructor_finished = false;
    bool release_destructor = false;

    uint8_t input_buffer[37] = {};
    probe.expected_input = input_buffer;
    probe.expected_input_size = sizeof(input_buffer);

    auto req = std::make_unique<InferRequest>();
    req->session_id = sid;
    req->model_id = "async_cleanup";
    req->inputs[0].data = input_buffer;
    req->inputs[0].byte_size = sizeof(input_buffer);
    req->inputs[0].dtype = HAL_DTYPE_UINT8;
    req->inputs[0].dma_fd = -1;
    req->num_inputs = 1;
    auto blocking_probe = std::make_shared<BlockingResourceProbe>(
        &resource_mu, &resource_cv, &destructor_started,
        &destructor_finished, &release_destructor);
    req->resource_holder = blocking_probe;
    req->on_complete = [&](int, HalTensor*, int, uint64_t, uint64_t, bool) {
        application_completions.fetch_add(1);
        std::lock_guard lock(completion_mu);
        completion_called = true;
        completion_cv.notify_all();
    };

    if (!scheduler.submit(std::move(req))) {
        {
            std::lock_guard lock(resource_mu);
            release_destructor = true;
        }
        resource_cv.notify_all();
        blocking_probe.reset();
        scheduler.stop();
        g_async_rollback_probe = nullptr;
        FAIL("scheduler rejected cleanup test request");
        return;
    }
    blocking_probe.reset();
    {
        std::unique_lock lock(completion_mu);
        const bool completed = completion_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return completion_called; });
        if (!completed) {
            {
                std::lock_guard resource_lock(resource_mu);
                release_destructor = true;
            }
            resource_cv.notify_all();
            scheduler.stop();
            g_async_rollback_probe = nullptr;
            FAIL("async completion did not run");
            return;
        }
    }
    {
        std::unique_lock lock(resource_mu);
        const bool started = resource_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return destructor_started; });
        if (!started) {
            release_destructor = true;
            lock.unlock();
            resource_cv.notify_all();
            scheduler.stop();
            g_async_rollback_probe = nullptr;
            FAIL("request resource destructor did not start");
            return;
        }
    }

    std::mutex drain_mu;
    std::condition_variable drain_cv;
    bool drain_waiting = false;
    scheduler.set_async_drain_wait_hook_for_test([&] {
        std::lock_guard lock(drain_mu);
        drain_waiting = true;
        drain_cv.notify_all();
    });

    std::atomic<bool> drain_done{false};
    int drain_result = -1;
    std::thread drain_thread([&] {
        drain_result = scheduler.drain_async(2000);
        drain_done.store(true, std::memory_order_release);
    });
    {
        std::unique_lock lock(drain_mu);
        const bool entered_wait = drain_cv.wait_for(
            lock, std::chrono::seconds(2), [&] { return drain_waiting; });
        if (!entered_wait) {
            {
                std::lock_guard resource_lock(resource_mu);
                release_destructor = true;
            }
            resource_cv.notify_all();
            lock.unlock();
            drain_thread.join();
            scheduler.stop();
            g_async_rollback_probe = nullptr;
            FAIL("drain did not enter its outstanding-work wait");
            return;
        }
    }
    const bool drain_waited = !drain_done.load(std::memory_order_acquire);

    {
        std::lock_guard lock(resource_mu);
        release_destructor = true;
    }
    resource_cv.notify_all();
    drain_thread.join();
    scheduler.stop();

    ASSERT_TRUE(drain_waited,
                "drain returned while request resource cleanup was blocked");
    ASSERT_EQ(drain_result, 0,
              "drain reported an orphan after cleanup completed");
    {
        std::lock_guard lock(resource_mu);
        ASSERT_TRUE(destructor_finished,
                    "drain returned before request resource destructor finished");
    }
    ASSERT_EQ(model_mgr.unregister_model("async_cleanup"), 0,
              "cleanup test leaked a model reference");
    g_async_rollback_probe = nullptr;

    PASS();
}

void test_inference_scheduler_stop_stress() {
    TEST(inference_scheduler_stop_stress);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager model_mgr(loader.infer_ops(), loader.post_ops(), loader.draw_ops(), &loader);
    SessionManager session_mgr;

    // Exercise the empty-queue shutdown edge repeatedly. A worker can be
    // between checking the wait predicate and blocking when stop() transitions
    // running_; without the transition synchronized by the queue mutex, the
    // notification can be lost and stop() hangs while joining that worker.
    constexpr int kIterations = 1000;
    constexpr int kWorkers = 4;
    for (int i = 0; i < kIterations; ++i) {
        InferenceScheduler scheduler(&model_mgr, &session_mgr, kWorkers, 1);
        scheduler.start();
        std::this_thread::yield();
        scheduler.stop();
    }

    PASS();
}

// ─── Test: DMA-BUF fd infer path (simulated) ────────────────────────────────

void test_dma_fd_infer() {
    TEST(dma_fd_infer_path);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager mgr(loader.infer_ops(), loader.post_ops(), loader.draw_ops(), &loader);
    int rc = mgr.register_model("dma_test", "/fake/model.hef");
    ASSERT_EQ(rc, 0, "register failed");

    auto snap = mgr.acquire_model_snapshot("dma_test");
    ASSERT_TRUE(snap.has_value(), "model not found");

    // Simulate DMA-BUF path: data=NULL, dma_fd=fake (stub ignores actual fd)
    HalTensor input{};
    input.data      = nullptr;
    input.dma_fd    = 42;   // fake fd; stub doesn't actually read it
    input.ndim      = 3;
    input.shape[0]  = 1080;
    input.shape[1]  = 1920;
    input.shape[2]  = 1;
    input.dtype     = HAL_DTYPE_UINT8;
    input.byte_size = 1920 * 1080;

    HalTensor output{};
    uint8_t dummy_out[256] = {};
    output.data = dummy_out;
    output.byte_size = sizeof(dummy_out);

    rc = mgr.infer(snap->infer_session, &input, 1, &output, 1);
    ASSERT_EQ(rc, 0, "infer with dma_fd should succeed (stub)");

    mgr.release_model("dma_test");

    PASS();
}

// ─── MiniFdPublisher: minimal fd-pub server for AutoInfer tests ──────────────
// Speaks just enough of the camera-daemon fd protocol (fd_protocol.h) for
// FdReceiver: accept one client, answer SUBSCRIBE with OK, then serve
// RELEASE/UNSUBSCRIBE messages while pushing memfd-backed NV12 frames with
// caller-chosen sequence numbers (push_sequence). The memfd stands in for a
// DMA-BUF: MappedNV12Frame::from_frame mmaps it exactly like a dmabuf.

namespace {

class MiniFdPublisher {
public:
    explicit MiniFdPublisher(const char* tag)
        : socket_path_("/tmp/ai-auto-infer-" + std::string(tag) + "-" +
                       std::to_string(getpid()) + ".sock") {
        listen_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (listen_fd_ < 0) fail("mini publisher socket");
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(),
                     sizeof(addr.sun_path) - 1);
        ::unlink(socket_path_.c_str());
        if (::bind(listen_fd_, reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) != 0)
            fail("mini publisher bind");
        if (::listen(listen_fd_, 1) != 0) fail("mini publisher listen");
        server_thread_ = std::thread(&MiniFdPublisher::run, this);
    }

    ~MiniFdPublisher() {
        {
            std::lock_guard lock(mu_);
            stopping_ = true;
            cv_.notify_all();
        }
        int client = client_fd_.load(std::memory_order_acquire);
        if (client >= 0) ::shutdown(client, SHUT_RDWR);
        if (listen_fd_ >= 0) ::shutdown(listen_fd_, SHUT_RDWR);
        if (server_thread_.joinable()) server_thread_.join();
        client = client_fd_.exchange(-1, std::memory_order_acq_rel);
        if (client >= 0) ::close(client);
        if (listen_fd_ >= 0) {
            ::close(listen_fd_);
            listen_fd_ = -1;
        }
        ::unlink(socket_path_.c_str());
    }

    const std::string& socket_path() const { return socket_path_; }

    // Queue one NV12 16x16 frame with the given sequence number. Waits
    // (bounded) for the client to subscribe first so frame ordering relative
    // to the pipeline's observations is deterministic.
    void push_sequence(uint64_t sequence) {
        std::unique_lock lock(mu_);
        if (!cv_.wait_for(lock, 5s, [&] { return subscribed_; })) {
            std::fprintf(stderr, "mini publisher: client never subscribed\n");
            std::_Exit(1);
        }
        pending_.push_back(sequence);
        cv_.notify_all();
    }

    bool wait_for_unsubscribe() {
        std::unique_lock lock(mu_);
        return cv_.wait_for(lock, 5s, [&] { return unsubscribe_received_; });
    }

private:
    [[noreturn]] static void fail(const char* operation) {
        std::perror(operation);
        std::_Exit(1);
    }

    // 16x16 NV12 in a single memfd: 256 bytes Y + 128 bytes UV. sizes[] splits
    // the two planes so MappedNV12Frame maps exactly 384 bytes (a compact
    // single-plane size would make it map past EOF and SIGBUS on touch).
    static int make_nv12_memfd() {
        constexpr size_t kY = 16 * 16;
        constexpr size_t kUV = 16 * 8;
        int fd = static_cast<int>(::syscall(
            SYS_memfd_create, "ai-auto-infer-nv12", MFD_CLOEXEC));
        if (fd < 0) fail("memfd_create");
        static const std::vector<uint8_t> pixels(kY + kUV, 0x80);
        if (::write(fd, pixels.data(), pixels.size()) !=
            static_cast<ssize_t>(pixels.size()))
            fail("memfd write");
        return fd;
    }

    void run() {
        int client = ::accept(listen_fd_, nullptr, nullptr);
        if (client < 0) return;
        client_fd_.store(client, std::memory_order_release);

        FdPubSubscribeMsg subscribe{};
        int fds[FD_PUB_MAX_FDS];
        int num_fds = 0;
        if (fd_pub_recvmsg(client, &subscribe, sizeof(subscribe), fds,
                           &num_fds, FD_PUB_MAX_FDS) <= 0)
            return;

        FdPubResponseMsg response{};
        response.hdr.type = FD_PUB_MSG_OK;
        response.hdr.size = sizeof(response);
        if (fd_pub_sendmsg(client, &response, sizeof(response), nullptr, 0) != 0)
            return;

        {
            std::lock_guard lock(mu_);
            subscribed_ = true;
            cv_.notify_all();
        }

        // Combined serve loop: drain RELEASE/UNSUBSCRIBE whenever they arrive
        // and send one queued frame per iteration (short poll so both kinds
        // interleave). Mirrors the proven FakeFdPublisher shape from the
        // fd_receiver deadlock suite.
        uint64_t next_frame_id = 7;
        for (;;) {
            pollfd poll_fd{client, POLLIN, 0};
            const int poll_rc = ::poll(&poll_fd, 1, 20);
            if (poll_rc < 0) return;
            if (poll_rc > 0 && (poll_fd.revents & POLLIN)) {
                FdPubMsgHeader header{};
                num_fds = 0;
                const int received = fd_pub_recvmsg(
                    client, &header, sizeof(header), fds, &num_fds,
                    FD_PUB_MAX_FDS);
                if (received < 0 && (errno == EAGAIN || errno == EINTR)) {
                    // Spurious poll wakeup; nothing was consumed.
                } else if (received <= 0 || num_fds != 0 ||
                           header.size < sizeof(header) ||
                           header.size > sizeof(FdPubReleaseMsg)) {
                    for (int i = 0; i < num_fds; ++i) ::close(fds[i]);
                    return;
                } else {
                    FdPubReleaseMsg message{};
                    message.hdr = header;
                    const size_t payload_size = header.size - sizeof(header);
                    if (payload_size > 0 &&
                        ::recv(client,
                               reinterpret_cast<char*>(&message) +
                                   sizeof(header),
                               payload_size, MSG_WAITALL) !=
                            static_cast<ssize_t>(payload_size)) {
                        return;
                    }
                    if (message.hdr.type == FD_PUB_MSG_RELEASE &&
                        message.hdr.size == sizeof(message)) {
                        std::lock_guard lock(mu_);
                        ++release_count_;
                        cv_.notify_all();
                    } else if (message.hdr.type == FD_PUB_MSG_UNSUBSCRIBE &&
                               message.hdr.size == sizeof(FdPubMsgHeader)) {
                        std::lock_guard lock(mu_);
                        unsubscribe_received_ = true;
                        cv_.notify_all();
                        return;
                    } else {
                        return;
                    }
                }
            }

            uint64_t sequence = 0;
            bool have_frame = false;
            bool stop = false;
            {
                std::lock_guard lock(mu_);
                if (!pending_.empty()) {
                    sequence = pending_.front();
                    pending_.pop_front();
                    have_frame = true;
                }
                stop = stopping_;
            }
            if (stop) return;
            if (!have_frame) continue;

            const int frame_fd = make_nv12_memfd();
            if (frame_fd < 0) return;
            FdPubFrameMsg frame{};
            frame.hdr.type    = FD_PUB_MSG_FRAME;
            frame.hdr.size    = sizeof(frame);
            frame.frame_id    = next_frame_id++;
            frame.timestamp_ns = 1000000;
            frame.sequence    = sequence;
            frame.width       = 16;
            frame.height      = 16;
            frame.format      = HAL_PIX_FMT_NV12;
            frame.num_planes  = 2;
            frame.strides[0]  = 16;
            frame.strides[1]  = 16;
            frame.sizes[0]    = 16 * 16;
            frame.sizes[1]    = 16 * 8;
            frame.num_fds     = 1;
            if (fd_pub_sendmsg(client, &frame, sizeof(frame), &frame_fd,
                               1) != 0) {
                ::close(frame_fd);
                return;
            }
            ::close(frame_fd);  // SCM_RIGHTS dup'ed into the client
        }
    }

    const std::string socket_path_;
    int listen_fd_ = -1;
    std::thread server_thread_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<uint64_t> pending_;
    bool subscribed_ = false;
    bool stopping_ = false;
    bool unsubscribe_received_ = false;
    int release_count_ = 0;
    std::atomic<int> client_fd_{-1};
};

}  // namespace

// ─── Test: AutoInfer lifecycle ────────────────────────────────────────────────
// Phase 1: a pipeline whose model does not exist fails start() promptly (the
//          startup handshake reports the failure; no full startup-timeout wait)
//          and never sets failed().
// Phase 2: a valid model against a publisher socket that does not exist fails
//          start() within a bounded time (subscribe retries + startup timeout)
//          and never sets failed().
// Phase 3: a live publisher drives real inferences: sequence 0 must infer
//          (regression for the old last_seq=0 sentinel that dropped the first
//          frame after every reconnect), a duplicate sequence must be dropped,
//          the next sequence must infer, and stop() must quiesce cleanly.
void test_auto_infer_lifecycle() {
    TEST(auto_infer_lifecycle);

    std::string path = get_stub_path();
    ASSERT_TRUE(!path.empty(), "stub library not found");

    HalMlLoader loader;
    ASSERT_TRUE(loader.load(path), "load failed");

    ModelManager mgr(loader.infer_ops(), loader.post_ops(), loader.draw_ops(),
                     &loader);
    SessionManager session_mgr;
    EventBusClient event_bus;  // never connected; auto-publish disabled below
    PostprocessPool postprocess_pool(1, 8);

    const auto make_config = [](const char* model_id) {
        Config cfg;
        cfg.auto_infer_enabled = true;
        cfg.event_bus_auto_publish = false;
        AutoInferPipeline pipe;
        pipe.model_id = model_id;
        pipe.stream_id = "cam0_main";
        pipe.fps = 0;  // no fps gate: isolates sequence handling
        cfg.auto_infer_pipelines.push_back(pipe);
        return cfg;
    };

    {  // Phase 1: missing model
        FdReceiver fd_receiver(
            "/tmp/ai-auto-infer-unused-" + std::to_string(getpid()) + ".sock");
        InferenceScheduler scheduler(&mgr, &session_mgr, 2, 8);
        // AutoInfer keeps a const& to the config; it must outlive the object.
        const Config cfg = make_config("no-such-model");
        AutoInfer auto_infer(&mgr, &fd_receiver, &event_bus, &scheduler,
                             &session_mgr, &postprocess_pool, cfg);
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(!auto_infer.start(),
                    "start() must fail for a missing model");
        ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 30s,
                    "missing-model failure should come from the startup "
                    "handshake, not the 10s timeout");
        ASSERT_FALSE(auto_infer.failed(),
                     "startup failure must not set failed()");
        auto_infer.stop();
        ASSERT_FALSE(auto_infer.failed(),
                     "stop() after a failed start must not set failed()");
    }

    ASSERT_EQ(mgr.register_model("ai_stub_model", "/fake/model.hef"), 0,
              "register failed");

    {  // Phase 2: valid model, dead publisher socket
        FdReceiver fd_receiver(
            "/tmp/ai-auto-infer-no-such-" + std::to_string(getpid()) + ".sock");
        InferenceScheduler scheduler(&mgr, &session_mgr, 2, 8);
        const Config cfg = make_config("ai_stub_model");
        AutoInfer auto_infer(&mgr, &fd_receiver, &event_bus, &scheduler,
                             &session_mgr, &postprocess_pool, cfg);
        const auto t0 = std::chrono::steady_clock::now();
        ASSERT_TRUE(!auto_infer.start(),
                    "start() must fail when the publisher socket is dead");
        ASSERT_TRUE(std::chrono::steady_clock::now() - t0 < 30s,
                    "dead-socket startup failure must stay bounded");
        ASSERT_FALSE(auto_infer.failed(),
                     "startup timeout must not set failed()");
        auto_infer.stop();
    }

    {  // Phase 3: live publisher
        MiniFdPublisher publisher("live");
        FdReceiver fd_receiver(publisher.socket_path());
        InferenceScheduler scheduler(&mgr, &session_mgr, 2, 8);
        scheduler.start();
        postprocess_pool.start();
        const Config cfg = make_config("ai_stub_model");
        AutoInfer auto_infer(&mgr, &fd_receiver, &event_bus, &scheduler,
                             &session_mgr, &postprocess_pool, cfg);
        ASSERT_TRUE(auto_infer.start(),
                    "start() must succeed against a live publisher");

        const auto infer_count = [&]() -> uint64_t {
            for (const auto& s : session_mgr.list_sessions()) {
                if (s->app_id == "system" && s->model_id == "ai_stub_model")
                    return s->infer_count.load();
            }
            return 0;
        };
        const auto wait_for_count = [&](uint64_t expected) {
            for (int i = 0; i < 100; ++i) {  // bounded at 10s
                if (infer_count() >= expected) return true;
                std::this_thread::sleep_for(100ms);
            }
            return false;
        };

        // Note: one inference may bump infer_count by 1 or 2 — the scheduler's
        // sync path records stats itself and AutoInfer's on_complete records
        // again — so the assertions below are delta-based.
        publisher.push_sequence(0);
        ASSERT_TRUE(wait_for_count(1),
                    "sequence-0 frame must infer (first-frame drop regression)");
        const uint64_t after_first = infer_count();
        publisher.push_sequence(0);  // duplicate: must be deduplicated
        std::this_thread::sleep_for(500ms);
        ASSERT_EQ(infer_count(), after_first,
                  "duplicate sequence must not infer");
        publisher.push_sequence(1);
        ASSERT_TRUE(wait_for_count(after_first + 1),
                    "next sequence must infer");
        ASSERT_FALSE(auto_infer.failed(), "live loop must not report failure");

        auto_infer.stop();
        ASSERT_TRUE(publisher.wait_for_unsubscribe(),
                    "publisher must observe UNSUBSCRIBE on stop");
        postprocess_pool.stop();
        scheduler.stop();
        ASSERT_EQ(scheduler.drain_async(2000), 0,
                  "no async jobs may remain after stop");
    }

    mgr.unregister_model("ai_stub_model");

    PASS();
}

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    set_log_level("warn");

    std::cerr << "=== ai-runtime C++ unit tests ===\n\n";

    test_hal_loader();
    test_model_manager();
    test_owner_scoped_unregister();
    test_force_unregister_all_is_atomic_when_busy();
    test_model_alias_refcount();
    test_stream_infer_config();
    test_session_manager();
    test_session_ids_are_unique_under_concurrency();
    test_session_creation_policy();
    test_session_concurrent_destroy();
    test_postprocess_pool_stop_stress();
    test_inference_scheduler();
    test_inference_scheduler_async_reject_rolls_back();
    test_inference_scheduler_async_throw_rolls_back();
    test_inference_scheduler_inline_callback_before_success_return();
    test_inference_scheduler_inline_callback_then_error();
    test_inference_scheduler_inline_callback_then_throw();
    test_inference_scheduler_submit_stop_race();
    test_inference_scheduler_external_async_drain();
    test_inference_scheduler_drain_waits_for_resource_cleanup();
    test_inference_scheduler_stop_stress();
    test_dma_fd_infer();
    test_auto_infer_lifecycle();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
