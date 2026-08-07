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
#include "log.h"

#include <cassert>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <unistd.h>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>

using namespace aipc::ai_runtime;

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

    // Register
    int rc = mgr.register_model("yolo_test", "/fake/model.hef");
    ASSERT_EQ(rc, 0, "register_model failed");

    // Duplicate register should fail
    rc = mgr.register_model("yolo_test", "/fake/model.hef");
    ASSERT_TRUE(rc != 0, "duplicate register should fail");

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

// ─── Main ────────────────────────────────────────────────────────────────────

int main() {
    set_log_level("warn");

    std::cerr << "=== ai-runtime C++ unit tests ===\n\n";

    test_hal_loader();
    test_model_manager();
    test_model_alias_refcount();
    test_session_manager();
    test_session_concurrent_destroy();
    test_inference_scheduler();
    test_dma_fd_infer();

    std::cerr << "\n=== Results: " << tests_passed << " passed, "
              << tests_failed << " failed ===\n";

    return tests_failed > 0 ? 1 : 0;
}
