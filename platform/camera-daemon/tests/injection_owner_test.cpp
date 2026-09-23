/**
 * @file injection_owner_test.cpp
 * @brief P2-13 lifecycle session — InjectionService owner-reclaim test.
 *
 * Hazard under test: session_owner_fd_ is a raw fd number and nothing
 * today checks it against a disconnect. An app killed mid-injection
 * leaves the session live forever; worse, the kernel recycles fd
 * numbers, so a NEW client can inherit the dead session's owner id.
 * release_owner(client_fd) is the disconnect hook — it closes the
 * session when (and only when) the disconnecting client is the owner,
 * the fd-keyed sweep FdPublisher::disconnect_client already runs for
 * outstanding frames and DSP registry buffers.
 *
 * Host-only harness: DspService runs with a fake HalDspOps (init/deinit
 * only) and a zeroed but non-null HalFrameBufferOps; import_buffer on a
 * memfd takes the non-dma mmap route (mem_type = HAL_MEM_MALLOC), so the
 * registry and pin plane work with no HAL hardware at all. That also
 * dictates the frame shape: push_frame demands HAL_MEM_DMABUF for NV12
 * (REPLACE), which a host memfd can never be — the test rides the ARGB32
 * OVERLAY route instead, the exact shape the SDK's memfd blend ring
 * pushes in production (injection_service.cpp:188 vs the argb path).
 */

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdint>
#include <cstring>

#include <sys/mman.h> /* memfd_create */
#include <unistd.h>   /* ftruncate, close */

#include "dsp_service.h"
#include "injection_service.h"

/* Fake DSP context: any non-null pointer satisfies start()'s check. */
static int fake_dsp_init(const HalDspConfig*, void** ctx) {
    *ctx = (void*)0x1;
    return 0;
}
static int fake_dsp_deinit(void*) { return 0; }

/* One memfd-backed plane file, truncated to `size`. -1 on failure. */
static int make_memfd(size_t size) {
    int fd = memfd_create("inj-owner-test", 0);
    if (fd < 0) return -1;
    if (ftruncate(fd, (off_t)size) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Import one 64x64 ARGB32 single-plane buffer for `client_fd`; returns
 * the registry id or aborts (this is a test, every failure is a failure).
 * 64*4-byte stride, one plane of 64 rows. */
static uint64_t import_argb(DspService& dsp, int client_fd) {
    const uint32_t strides[1] = {64 * 4};
    const uint32_t sizes[1] = {64 * 4 * 64};
    int fds[1] = {make_memfd(sizes[0])};
    assert(fds[0] >= 0);
    auto r = dsp.import_buffer(client_fd, 64, 64, HAL_PIX_FMT_ARGB32, 1,
                               strides, sizes, fds);
    close(fds[0]); /* registry dup'd its own copy */
    assert(r.rc == DSP_SVC_OK);
    assert(r.id != 0);
    return r.id;
}

static InjectionPushResult push_overlay(InjectionService& inj,
                                        uint64_t buffer_id,
                                        const char* session_id = nullptr) {
    InjectionFrameDesc d;
    d.buffer_id = buffer_id;
    d.width = 64;
    d.height = 64;
    d.stride = 64 * 4; /* ARGB32 byte stride (>= width*4) */
    d.mode = InjectionMode::Overlay;
    d.stream_id = "sub";
    if (session_id) d.session_id = session_id;
    return inj.push_frame(d);
}

int main() {
    HalDspOps dsp_ops = {};
    dsp_ops.init = fake_dsp_init;
    dsp_ops.deinit = fake_dsp_deinit;
    HalFrameBufferOps fb_ops = {}; /* non-null gate; never dereferenced
                                      on the imported-buffer route */
    DspService dsp(&dsp_ops, &fb_ops);
    assert(dsp.start());
    assert(dsp.is_running());

    InjectionServiceConfig cfg;
    cfg.enabled = true;
    InjectionService inj(&dsp, cfg);
    assert(inj.start());
    assert(inj.is_running());

    /* 1. Owner 7 pushes — session opens, owner recorded. */
    uint64_t buf_a = import_argb(dsp, 7);
    InjectionPushResult r = push_overlay(inj, buf_a);
    assert(r.rc == INJ_SVC_OK);
    {
        InjectionServiceStatus st = inj.status();
        assert(st.active);
        assert(st.session_owner_fd == 7);
        assert(st.queue_depth == 1);
    }

    /* 2. release_owner with a MISMATCHED fd is a no-op — a different
     * client disconnecting must not close the live session. */
    inj.release_owner(999);
    {
        InjectionServiceStatus st = inj.status();
        assert(st.active);
        assert(st.session_owner_fd == 7);
        assert(st.queue_depth == 1);
    }

    /* 3. release_owner(7) — the owner's disconnect closes the session,
     * flushes the queue (pins released) and frees the owner slot. */
    inj.release_owner(7);
    {
        InjectionServiceStatus st = inj.status();
        assert(!st.active);
        assert(st.session_owner_fd == -1);
        assert(st.queue_depth == 0);
    }

    /* 4. The service is not wedged: a new client (fd 8) imports and
     * pushes; a fresh session opens under the new owner. */
    uint64_t buf_b = import_argb(dsp, 8);
    InjectionPushResult r2 = push_overlay(inj, buf_b);
    assert(r2.rc == INJ_SVC_OK);
    {
        InjectionServiceStatus st = inj.status();
        assert(st.active);
        assert(st.session_owner_fd == 8);
    }

    /* 5. Explicit close still works after a reclaim cycle. */
    inj.stop_injection();
    {
        InjectionServiceStatus st = inj.status();
        assert(!st.active);
    }

    /* 6. Session tag (P2-13): the tag is observability, not ownership —
     * the owner fd stays the anchor. The session records the LATEST
     * non-empty request tag; an untagged frame does not clear it (the
     * session still belongs to whoever opened it); close clears it. */
    {
        InjectionPushResult rt = push_overlay(inj, buf_b, "s1");
        assert(rt.rc == INJ_SVC_OK);
        assert(inj.status().session_id == "s1"); /* tag at open */

        rt = push_overlay(inj, buf_b); /* untagged follow-up */
        assert(rt.rc == INJ_SVC_OK);
        assert(inj.status().session_id == "s1"); /* not cleared */

        rt = push_overlay(inj, buf_b, "s2");
        assert(rt.rc == INJ_SVC_OK);
        assert(inj.status().session_id == "s2"); /* latest non-empty wins */

        inj.release_owner(8); /* owner disconnect reclaims + clears tag */
        InjectionServiceStatus st = inj.status();
        assert(!st.active);
        assert(st.session_id.empty());
    }

    /* 7. Write-lease in-flight set (Fix-1): status() reports queued +
     * mid-bake ids; take_frame moves the chosen id queue->baking;
     * note_bake_done releases it. A session close (EOS) that lands
     * mid-bake keeps the baking id reported — the SDK must not recycle
     * a slot the daemon is still reading. */
    {
        const uint64_t pool[3] = {import_argb(dsp, 9), import_argb(dsp, 9),
                                  import_argb(dsp, 9)};

        /* All queued: the snapshot lists every id exactly once. */
        for (int i = 0; i < 3; ++i)
            assert(push_overlay(inj, pool[i]).rc == INJ_SVC_OK);
        InjectionServiceStatus st = inj.status();
        assert(st.in_flight_buffer_ids.size() == 3);
        for (int i = 0; i < 3; ++i)
            assert(std::count(st.in_flight_buffer_ids.begin(),
                              st.in_flight_buffer_ids.end(),
                              pool[i]) == 1);

        /* Newest-due-wins take: the two superseded older frames drop
         * out of the set; the chosen one MOVES to baking — still
         * in flight until the bake site acks. */
        InjectionService::QueuedFrame qf;
        assert(inj.take_frame("sub", 64, 64, 0, qf));
        assert(qf.buffer_id == pool[2]);
        st = inj.status();
        assert(st.in_flight_buffer_ids.size() == 1);
        assert(st.in_flight_buffer_ids[0] == pool[2]);
        assert(st.frames_injected == 1);
        assert(st.frames_dropped == 2);

        /* Bake-done ack: the slot becomes reusable. */
        inj.note_bake_done(qf.buffer_id);
        assert(inj.status().in_flight_buffer_ids.empty());

        /* EOS landing mid-bake: the queued frame flushes out of the
         * set, but a compose already handed out stays reported until
         * its own ack (wrong-direction safety: leak, never a tear). */
        assert(push_overlay(inj, pool[0]).rc == INJ_SVC_OK);
        InjectionService::QueuedFrame mid;
        assert(inj.take_frame("sub", 64, 64, 0, mid));
        assert(push_overlay(inj, pool[1]).rc == INJ_SVC_OK);
        InjectionFrameDesc eos;
        eos.end_of_stream = true;
        assert(inj.push_frame(eos).rc == INJ_SVC_OK);
        st = inj.status();
        assert(!st.active);
        assert(st.in_flight_buffer_ids.size() == 1); /* mid-bake only */
        assert(st.in_flight_buffer_ids[0] == mid.buffer_id);
        inj.note_bake_done(mid.buffer_id);
        assert(inj.status().in_flight_buffer_ids.empty());

        /* Ack of an unknown id is a harmless no-op. */
        inj.note_bake_done(0xdeadbeefULL);
    }

    inj.stop();
    dsp.release_client_buffers(7);
    dsp.release_client_buffers(8);
    dsp.release_client_buffers(9);
    dsp.stop();

    std::printf("injection_owner_test: all assertions passed\n");
    return 0;
}
