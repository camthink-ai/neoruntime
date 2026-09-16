#include "stream_infer_utils.h"
#include "common/hal_common.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>
#include <unordered_set>
#include <vector>

#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

using aipc::ai_runtime::FrameRateGate;
using aipc::ai_runtime::ReceivedFrame;
using aipc::ai_runtime::StreamAdmissionController;
using aipc::ai_runtime::bind_stream_nv12_input;
using aipc::ai_runtime::apply_result_filters;
using aipc::ai_runtime::valid_stream_class_filter_size;
using aipc::ai_runtime::valid_stream_fps;
using aipc::ai_runtime::valid_stream_id;
using aipc::ai_runtime::valid_stream_min_confidence;
namespace pb = aipc::inference;

namespace {

HalDmaFrameDesc captured_desc{};
int bind_calls = 0;
int free_calls = 0;

int fake_bind_dma_frame(HalInferenceSession*, const HalDmaFrameDesc* desc,
                        HalTensor* out) {
    ++bind_calls;
    captured_desc = *desc;
    *out = {};
    const uint64_t total_bytes =
        static_cast<uint64_t>(desc->bytes_used[0]) + desc->bytes_used[1];
    assert(total_bytes <= std::numeric_limits<uint32_t>::max());
    out->byte_size = static_cast<uint32_t>(total_bytes);
    out->priv = reinterpret_cast<void*>(1);
    return HAL_OK;
}

void fake_free_tensor(HalTensor* tensor) {
    ++free_calls;
    tensor->priv = nullptr;
}

ReceivedFrame compact_nv12_frame(uint32_t num_planes, size_t num_fds) {
    ReceivedFrame frame{};
    frame.width = 16;
    frame.height = 16;
    frame.format = HAL_PIX_FMT_NV12;
    frame.num_planes = num_planes;
    frame.strides[0] = 16;
    frame.strides[1] = 16;
    frame.sizes[0] = num_planes == 1 ? 16 * 16 * 3 / 2 : 16 * 16;
    frame.sizes[1] = 16 * 8;
    frame.fd_group = std::make_shared<aipc::ai_runtime::FdGroup>();
    for (size_t i = 0; i < num_fds; ++i) {
        const int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        assert(fd >= 0);
        frame.fd_group->fds.push_back(fd);
    }
    return frame;
}

void expect_invalid_nv12_frame(ReceivedFrame frame) {
    HalInferenceOps ops{};
    ops.bind_dma_frame = fake_bind_dma_frame;
    ops.free_tensor = fake_free_tensor;
    bind_calls = 0;
    free_calls = 0;
    int bind_status = HAL_OK;

    auto lease = bind_stream_nv12_input(
        reinterpret_cast<HalInferenceSession*>(1), &ops, frame, &bind_status);
    assert(!lease);
    assert(bind_status == HAL_ERR_INVALID_ARG);
    assert(bind_calls == 0);
    assert(free_calls == 0);
}

void test_request_validation() {
    assert(valid_stream_fps(0));
    assert(valid_stream_fps(120));
    assert(!valid_stream_fps(121));
    assert(!valid_stream_fps(std::numeric_limits<uint32_t>::max()));

    assert(valid_stream_min_confidence(0.0f));
    assert(valid_stream_min_confidence(1.0f));
    assert(!valid_stream_min_confidence(-0.1f));
    assert(!valid_stream_min_confidence(1.1f));
    assert(!valid_stream_min_confidence(
        std::numeric_limits<float>::quiet_NaN()));

    assert(valid_stream_class_filter_size(0));
    assert(valid_stream_class_filter_size(256));
    assert(!valid_stream_class_filter_size(257));

    assert(valid_stream_id("cam0_main"));
    assert(valid_stream_id("camera-1"));
    assert(!valid_stream_id(""));
    assert(!valid_stream_id(std::string("cam0\0main", 9)));
    assert(!valid_stream_id("cam0/main"));
    assert(!valid_stream_id(std::string(FD_PUB_MAX_STREAM_NAME, 'a')));
}

void test_class_filter_accepts_legacy_packed_varints() {
    // field 8, length-delimited packed int32 values [1, 2, 3]
    const std::string encoded("\x42\x03\x01\x02\x03", 5);
    pb::StreamInferRequest request;
    assert(request.ParseFromString(encoded));
    assert(request.class_filter_size() == 3);
    assert(request.class_filter(0) == 1);
    assert(request.class_filter(1) == 2);
    assert(request.class_filter(2) == 3);
}

void test_dual_fd_nv12_binding_and_lifetime() {
    int pipe_fds[2] = {-1, -1};
    assert(pipe(pipe_fds) == 0);

    auto fd_group = std::make_shared<aipc::ai_runtime::FdGroup>();
    fd_group->fds = {pipe_fds[0], pipe_fds[1]};

    ReceivedFrame frame{};
    frame.width = 16;
    frame.height = 16;
    frame.format = HAL_PIX_FMT_NV12;
    frame.num_planes = 2;
    frame.strides[0] = 16;
    frame.strides[1] = 16;
    frame.sizes[0] = 16 * 16;
    frame.sizes[1] = 16 * 8;
    frame.fd_group = fd_group;

    HalInferenceOps ops{};
    ops.bind_dma_frame = fake_bind_dma_frame;
    ops.free_tensor = fake_free_tensor;
    bind_calls = 0;
    free_calls = 0;

    int bind_status = HAL_ERROR;
    auto lease = bind_stream_nv12_input(
        reinterpret_cast<HalInferenceSession*>(1), &ops, frame, &bind_status);
    assert(lease != nullptr);
    assert(bind_status == HAL_OK);
    assert(bind_calls == 1);
    assert(free_calls == 0);
    assert(captured_desc.fd[0] == pipe_fds[0]);
    assert(captured_desc.fd[1] == pipe_fds[1]);
    assert(captured_desc.bytes_used[0] == 16u * 16u);
    assert(captured_desc.bytes_used[1] == 16u * 8u);
    assert(captured_desc.borrowed == 1);
    assert(lease->tensor().byte_size == 16u * 16u * 3u / 2u);

    frame.fd_group.reset();
    fd_group.reset();
    assert(fcntl(pipe_fds[0], F_GETFD) != -1);
    assert(fcntl(pipe_fds[1], F_GETFD) != -1);

    lease.reset();
    assert(free_calls == 1);
    errno = 0;
    assert(fcntl(pipe_fds[0], F_GETFD) == -1 && errno == EBADF);
    errno = 0;
    assert(fcntl(pipe_fds[1], F_GETFD) == -1 && errno == EBADF);
}

void test_single_fd_compact_nv12_binding() {
    auto frame = compact_nv12_frame(1, 1);

    HalInferenceOps ops{};
    ops.bind_dma_frame = fake_bind_dma_frame;
    ops.free_tensor = fake_free_tensor;
    bind_calls = 0;
    free_calls = 0;

    int bind_status = HAL_ERROR;
    auto lease = bind_stream_nv12_input(
        reinterpret_cast<HalInferenceSession*>(1), &ops, frame, &bind_status);
    assert(lease);
    assert(bind_status == HAL_OK);
    assert(bind_calls == 1);
    assert(captured_desc.fd[0] == frame.fd_group->fds[0]);
    assert(captured_desc.fd[1] == -1);
    assert(captured_desc.stride[0] == 16);
    assert(captured_desc.bytes_used[0] == 16u * 16u * 3u / 2u);
    lease.reset();
    assert(free_calls == 1);
}

void test_nv12_binding_rejects_odd_dimensions() {
    auto odd_width = compact_nv12_frame(1, 1);
    odd_width.width = 15;
    odd_width.strides[0] = 15;
    odd_width.sizes[0] = 15 * 16 * 3 / 2;
    expect_invalid_nv12_frame(std::move(odd_width));

    auto odd_height = compact_nv12_frame(1, 1);
    odd_height.height = 15;
    odd_height.sizes[0] = 16 * 15 * 3 / 2;
    expect_invalid_nv12_frame(std::move(odd_height));
}

void test_nv12_binding_rejects_malformed_topology() {
    for (const auto [num_planes, num_fds] :
         {std::pair<uint32_t, size_t>{0, 1}, {1, 2}, {2, 1}, {2, 3}, {3, 2}}) {
        expect_invalid_nv12_frame(compact_nv12_frame(num_planes, num_fds));
    }

    auto invalid_single_fd = compact_nv12_frame(1, 1);
    close(invalid_single_fd.fd_group->fds[0]);
    invalid_single_fd.fd_group->fds[0] = -1;
    expect_invalid_nv12_frame(std::move(invalid_single_fd));

    auto invalid_dual_fd = compact_nv12_frame(2, 2);
    close(invalid_dual_fd.fd_group->fds[1]);
    invalid_dual_fd.fd_group->fds[1] = -1;
    expect_invalid_nv12_frame(std::move(invalid_dual_fd));
}

void test_nv12_binding_rejects_noncompact_strides() {
    for (uint32_t stride : {0u, 15u, 17u}) {
        auto frame = compact_nv12_frame(1, 1);
        frame.strides[0] = stride;
        expect_invalid_nv12_frame(std::move(frame));
    }

    auto short_y_stride = compact_nv12_frame(2, 2);
    short_y_stride.strides[0] = 15;
    expect_invalid_nv12_frame(std::move(short_y_stride));

    auto padded_uv_stride = compact_nv12_frame(2, 2);
    padded_uv_stride.strides[1] = 17;
    expect_invalid_nv12_frame(std::move(padded_uv_stride));
}

void test_nv12_binding_rejects_undersized_planes() {
    auto single = compact_nv12_frame(1, 1);
    single.sizes[0] = 383;
    expect_invalid_nv12_frame(std::move(single));

    auto dual_y = compact_nv12_frame(2, 2);
    dual_y.sizes[0] = 255;
    expect_invalid_nv12_frame(std::move(dual_y));

    auto dual_uv = compact_nv12_frame(2, 2);
    dual_uv.sizes[1] = 127;
    expect_invalid_nv12_frame(std::move(dual_uv));
}

void test_nv12_binding_rejects_total_byte_overflow() {
    auto over_u32 = compact_nv12_frame(1, 1);
    over_u32.width = 65536;
    over_u32.height = 43692;
    over_u32.strides[0] = over_u32.width;
    over_u32.sizes[0] = std::numeric_limits<uint32_t>::max();
    expect_invalid_nv12_frame(std::move(over_u32));

    auto wrapped = compact_nv12_frame(1, 1);
    wrapped.width = 0xfffffffcu;
    wrapped.height = 0xaaaaaaaeu;
    wrapped.strides[0] = wrapped.width;
    wrapped.sizes[0] = std::numeric_limits<uint32_t>::max();
    expect_invalid_nv12_frame(std::move(wrapped));
}

void test_unlimited_gate() {
    FrameRateGate gate(0);
    const auto now = FrameRateGate::TimePoint{};
    assert(gate.allow(now));
    assert(gate.allow(now));
}

void test_positive_gate_has_no_burst_credit() {
    using namespace std::chrono_literals;

    FrameRateGate gate(10);
    const auto start = FrameRateGate::TimePoint{};
    assert(gate.allow(start));
    assert(!gate.allow(start + 99ms));
    assert(gate.allow(start + 150ms));
    assert(!gate.allow(start + 200ms));
    assert(gate.allow(start + 250ms));
}

void test_result_filters() {
    pb::PostResult result;

    auto* first = result.add_detections();
    first->set_class_id(7);
    first->set_confidence(0.8f);
    first->set_label("first");

    auto* stable = result.add_detections();
    stable->set_class_id(7);
    stable->set_confidence(0.8f);
    stable->set_label("stable");

    auto* best = result.add_detections();
    best->set_class_id(7);
    best->set_confidence(0.9f);
    best->set_label("best");

    auto* wrong_class = result.add_detections();
    wrong_class->set_class_id(8);
    wrong_class->set_confidence(1.0f);

    auto* nan_detection = result.add_detections();
    nan_detection->set_class_id(7);
    nan_detection->set_confidence(std::numeric_limits<float>::quiet_NaN());

    for (float confidence : {0.6f, 0.9f, 0.7f}) {
        auto* classification = result.add_classifications();
        classification->set_class_id(7);
        classification->set_confidence(confidence);
        classification->set_label("classification");
    }

    for (float confidence : {0.6f, 0.9f, 0.7f}) {
        auto* mask = result.add_masks();
        mask->set_class_id(7);
        mask->set_confidence(confidence);
        mask->set_label("mask");
    }

    auto* nan_mask = result.add_masks();
    nan_mask->set_class_id(7);
    nan_mask->set_confidence(std::numeric_limits<float>::infinity());

    for (float confidence : {0.6f, 0.9f, 0.7f}) {
        result.add_ocr_lines()->set_confidence(confidence);
    }
    auto* nan_line = result.add_ocr_lines();
    nan_line->set_confidence(std::numeric_limits<float>::quiet_NaN());

    const bool nonempty = apply_result_filters(
        &result, 2, 0.5f, std::unordered_set<int32_t>{7}, true);

    assert(nonempty);
    assert(result.detections_size() == 2);
    assert(result.detections(0).confidence() == 0.9f);
    assert(result.detections(1).confidence() == 0.8f);
    assert(result.detections(1).label().empty());
    assert(result.classifications_size() == 3);
    assert(result.classifications(0).confidence() == 0.6f);
    assert(result.classifications(0).label().empty());
    assert(result.masks_size() == 3);
    assert(result.masks(0).confidence() == 0.6f);
    assert(result.masks(0).label().empty());
    assert(result.ocr_lines_size() == 3);
    assert(result.ocr_lines(0).confidence() == 0.6f);
}

void test_stream_rpc_admission_limits_and_release() {
    StreamAdmissionController admission(2, 1, 1, 2, 1);

    auto first = admission.try_acquire_rpc("peer-a", "cam0_main");
    assert(first);
    assert(!admission.try_acquire_rpc("peer-a", "cam1_main"));
    assert(!admission.try_acquire_rpc("peer-b", "cam0_main"));

    auto second = admission.try_acquire_rpc("peer-b", "cam1_main");
    assert(second);
    assert(!admission.try_acquire_rpc("peer-c", "cam2_main"));

    first.reset();
    auto replacement = admission.try_acquire_rpc("peer-c", "cam0_main");
    assert(replacement);
}

void test_zero_admission_limits_are_unbounded() {
    StreamAdmissionController admission(0, 0, 0, 0, 0);
    std::vector<std::shared_ptr<StreamAdmissionController::RpcPermit>> rpc_permits;
    std::vector<std::shared_ptr<StreamAdmissionController::WorkPermit>> work_permits;
    auto local = std::make_shared<std::atomic<uint32_t>>(0);

    for (int i = 0; i < 64; ++i) {
        auto rpc = admission.try_acquire_rpc("same-peer", "same-stream");
        assert(rpc);
        rpc_permits.push_back(std::move(rpc));

        auto work = admission.try_acquire_work(local);
        assert(work);
        work_permits.push_back(std::move(work));
    }
    assert(local->load() == work_permits.size());

    work_permits.clear();
    rpc_permits.clear();
    assert(local->load() == 0);
}

void test_stream_work_admission_limits_and_release() {
    StreamAdmissionController admission(8, 8, 8, 2, 1);
    auto first_rpc = std::make_shared<std::atomic<uint32_t>>(0);
    auto second_rpc = std::make_shared<std::atomic<uint32_t>>(0);
    auto third_rpc = std::make_shared<std::atomic<uint32_t>>(0);

    auto first = admission.try_acquire_work(first_rpc);
    assert(first);
    assert(!admission.try_acquire_work(first_rpc));
    auto second = admission.try_acquire_work(second_rpc);
    assert(second);
    assert(!admission.try_acquire_work(third_rpc));

    first.reset();
    auto replacement = admission.try_acquire_work(third_rpc);
    assert(replacement);
    assert(first_rpc->load() == 0);
    replacement.reset();
    second.reset();
    assert(second_rpc->load() == 0);
    assert(third_rpc->load() == 0);
}

void test_stream_work_admission_never_oversubscribes() {
    constexpr uint32_t kLimit = 3;
    StreamAdmissionController admission(8, 8, 8, kLimit, kLimit);
    std::atomic<uint32_t> active{0};
    std::atomic<uint32_t> peak{0};
    std::atomic<bool> release{false};
    std::vector<std::thread> threads;

    for (int i = 0; i < 16; ++i) {
        threads.emplace_back([&] {
            auto local = std::make_shared<std::atomic<uint32_t>>(0);
            auto permit = admission.try_acquire_work(local);
            if (!permit) return;
            const uint32_t now = active.fetch_add(1) + 1;
            uint32_t observed = peak.load();
            while (observed < now &&
                   !peak.compare_exchange_weak(observed, now)) {
            }
            while (!release.load()) std::this_thread::yield();
            active.fetch_sub(1);
        });
    }
    release.store(true);
    for (auto& thread : threads) thread.join();
    assert(peak.load() <= kLimit);
}

void test_empty_after_nonfinite_filter() {
    pb::PostResult result;
    auto* classification = result.add_classifications();
    classification->set_class_id(1);
    classification->set_confidence(
        std::numeric_limits<float>::quiet_NaN());

    assert(!apply_result_filters(
        &result, 0, 0.0f, std::unordered_set<int32_t>{}, false));
    assert(result.classifications_size() == 0);
}

int failing_bind_rc = HAL_ERR_NO_MEM;

int failing_bind_dma_frame(HalInferenceSession*, const HalDmaFrameDesc* desc,
                           HalTensor*) {
    ++bind_calls;
    captured_desc = *desc;
    return failing_bind_rc;
}

// Backend rejects the descriptor: the exact backend status must reach the
// caller, free_tensor must never run (nothing was bound), the returned lease
// must be null, and the caller's borrowed fds must remain untouched (the
// backend took no ownership on failure). Covered for both NV12 topologies.
void test_backend_bind_failure_propagates_status() {
    for (const auto [num_planes, num_fds] :
         {std::pair<uint32_t, size_t>{1, 1}, {2, 2}}) {
        auto frame = compact_nv12_frame(num_planes, num_fds);

        HalInferenceOps ops{};
        ops.bind_dma_frame = failing_bind_dma_frame;
        ops.free_tensor = fake_free_tensor;
        bind_calls = 0;
        free_calls = 0;

        int bind_status = HAL_OK;
        auto lease = bind_stream_nv12_input(
            reinterpret_cast<HalInferenceSession*>(1), &ops, frame,
            &bind_status);
        assert(lease == nullptr);                  // null lease on failure
        assert(bind_status == failing_bind_rc);    // exact status propagated
        assert(bind_calls == 1);                   // backend was invoked once
        assert(free_calls == 0);                   // nothing bound, nothing freed
        for (int fd : frame.fd_group->fds) {
            assert(fd >= 0);
            assert(fcntl(fd, F_GETFD) != -1);      // caller fds stay open
        }
    }
}

// Full-descriptor assertions: every field of the HalDmaFrameDesc handed to
// the backend, including untouched planes (fd==-1, stride/bytes_used==0) and
// the offset[]=0 contract — not just the happy-path fields.
void test_descriptor_fields_fully_specified() {
    {
        auto frame = compact_nv12_frame(1, 1);
        HalInferenceOps ops{};
        ops.bind_dma_frame = fake_bind_dma_frame;
        ops.free_tensor = fake_free_tensor;
        bind_calls = 0;

        int bind_status = HAL_ERROR;
        auto lease = bind_stream_nv12_input(
            reinterpret_cast<HalInferenceSession*>(1), &ops, frame,
            &bind_status);
        assert(lease && bind_status == HAL_OK && bind_calls == 1);
        const HalDmaFrameDesc& d = captured_desc;
        assert(d.format == HAL_PIX_FMT_NV12);
        assert(d.width == 16 && d.height == 16);
        assert(d.borrowed == 1);
        assert(d.fd[0] == frame.fd_group->fds[0]);
        for (int i = 1; i < HAL_MAX_PLANES; i++) assert(d.fd[i] == -1);
        for (int i = 0; i < HAL_MAX_PLANES; i++) assert(d.offset[i] == 0);
        assert(d.stride[0] == 16);
        for (int i = 1; i < HAL_MAX_PLANES; i++) assert(d.stride[i] == 0);
        assert(d.bytes_used[0] == 16u * 16u * 3u / 2u);
        for (int i = 1; i < HAL_MAX_PLANES; i++) assert(d.bytes_used[i] == 0);
    }
    {
        auto frame = compact_nv12_frame(2, 2);
        HalInferenceOps ops{};
        ops.bind_dma_frame = fake_bind_dma_frame;
        ops.free_tensor = fake_free_tensor;
        bind_calls = 0;

        int bind_status = HAL_ERROR;
        auto lease = bind_stream_nv12_input(
            reinterpret_cast<HalInferenceSession*>(1), &ops, frame,
            &bind_status);
        assert(lease && bind_status == HAL_OK && bind_calls == 1);
        const HalDmaFrameDesc& d = captured_desc;
        assert(d.format == HAL_PIX_FMT_NV12);
        assert(d.width == 16 && d.height == 16);
        assert(d.borrowed == 1);
        assert(d.fd[0] == frame.fd_group->fds[0]);
        assert(d.fd[1] == frame.fd_group->fds[1]);
        for (int i = 2; i < HAL_MAX_PLANES; i++) assert(d.fd[i] == -1);
        for (int i = 0; i < HAL_MAX_PLANES; i++) assert(d.offset[i] == 0);
        assert(d.stride[0] == 16 && d.stride[1] == 16);
        for (int i = 2; i < HAL_MAX_PLANES; i++) assert(d.stride[i] == 0);
        assert(d.bytes_used[0] == 16u * 16u);
        assert(d.bytes_used[1] == 16u * 8u);
        for (int i = 2; i < HAL_MAX_PLANES; i++) assert(d.bytes_used[i] == 0);
    }
}

}  // namespace

int main() {
    test_request_validation();
    test_class_filter_accepts_legacy_packed_varints();
    test_dual_fd_nv12_binding_and_lifetime();
    test_single_fd_compact_nv12_binding();
    test_nv12_binding_rejects_odd_dimensions();
    test_nv12_binding_rejects_malformed_topology();
    test_nv12_binding_rejects_noncompact_strides();
    test_nv12_binding_rejects_undersized_planes();
    test_nv12_binding_rejects_total_byte_overflow();
    test_unlimited_gate();
    test_positive_gate_has_no_burst_credit();
    test_stream_rpc_admission_limits_and_release();
    test_zero_admission_limits_are_unbounded();
    test_stream_work_admission_limits_and_release();
    test_stream_work_admission_never_oversubscribes();
    test_result_filters();
    test_empty_after_nonfinite_filter();
    test_backend_bind_failure_propagates_status();
    test_descriptor_fields_fully_specified();
    std::cout << "stream infer utils: all tests passed\n";
    return 0;
}
