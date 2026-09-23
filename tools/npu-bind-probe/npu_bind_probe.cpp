/**
 * npu-bind-probe — read-only adjudicator for NPU input-buffer binding paths.
 *
 * The HailoRT 5.3.0 manual documents three ways to feed an inference input:
 *   A. Bindings::InferStream::set_pix_buffer(hailo_pix_buffer_t) with
 *      memory_type = HAILO_PIX_BUFFER_MEMORY_TYPE_DMABUF
 *      — infer_model.hpp:118 says "Currently only support … USERPTR",
 *        but vendor medialib claims DMABUF in production. CONTRADICTION.
 *   B. Bindings::InferStream::set_dma_buffer({fd, size})
 *      — single fd; whether an NV12 [1,H,W,3] input accepts one compact
 *        dmabuf of size W*H*3/2 is undocumented.
 *   C. Low-level InputStream::write_async(dmabuf_fd, size, cb)
 *      — explicitly experimental.
 *
 * This probe allocates compact NV12 dmabufs via /dev/dma_heap, fills them
 * with a deterministic pattern, runs each path against the given HEF, and
 * byte-compares every output tensor against a CPU set_buffer() baseline.
 * It creates no files and changes no system state; the VDevice is opened
 * exactly like HAL does (group "device0", multi_process_service,
 * ROUND_ROBIN) so it coexists with camera-daemon.
 *
 * Also answers (plan P0-1/P0-7):
 *   - create_bindings() cost  → decides ping-pong bindings depth
 *   - 2-bindings concurrent submit → cross-bindings safety signal
 *   - consistency loop  → CPU-written dmabuf read coherently by NPU
 *     (with and without DMA_BUF_IOCTL_SYNC; approximates the DSP-write →
 *     NPU-read fence question until the real daemon pool is exercised)
 *
 * Verdict lines are `[PROBE] path=<name> verdict=…` for machine scraping.
 */

#include <hailo/hailort.hpp>
#include <hailo/hailort.h>

#include <linux/dma-heap.h>
#include <linux/dma-buf.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

using hailort::ConfiguredInferModel;
using hailort::Expected;
using hailort::InferModel;
using hailort::InputStream;
using hailort::OutputStream;
using hailort::VDevice;
/* nested types — Bindings only exists inside ConfiguredInferModel */
using Bindings = hailort::ConfiguredInferModel::Bindings;

// ------------------------------------------------------------------ timing —

static uint64_t now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// ------------------------------------------------------------------ hashing —

static uint64_t fnv1a(const void *data, size_t len)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++)
    {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

// ------------------------------------------------------------- dma-heap i/f —

static const char *const kHeapCandidates[] = {
    /* rig 93.72 (hailo15) heaps, verified 2026-09-15 */
    "/dev/dma_heap/linux,cma",
    "/dev/dma_heap/hailo_media_buf,cma", /* camera-daemon medialib pool heap */
    /* generic names on other kernels */
    "/dev/dma_heap/system",
    "/dev/dma_heap/system-uncached",
    "/dev/dma_heap/CMA",
};

/** One dma_heap-backed buffer: dmabuf fd + CPU mapping of the whole object. */
struct DmaBuf
{
    int fd = -1;
    size_t size = 0;
    void *map = MAP_FAILED;

    ~DmaBuf()
    {
        if (map != MAP_FAILED)
            munmap(map, size);
        if (fd >= 0)
            close(fd);
    }
};

static int open_dma_heap(const char *override)
{
    if (override)
    {
        int fd = open(override, O_RDWR | O_CLOEXEC);
        if (fd < 0)
            fprintf(stderr, "[PROBE] heap open failed: %s (%s)\n", override, strerror(errno));
        return fd;
    }
    for (const char *path : kHeapCandidates)
    {
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            printf("[PROBE] heap=%s\n", path);
            return fd;
        }
    }
    return -1;
}

static bool dmabuf_alloc(DmaBuf *out, int heap_fd, size_t len)
{
    struct dma_heap_allocation_data data = {};
    data.len = len;
    data.fd_flags = O_RDWR | O_CLOEXEC;
    if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) != 0)
    {
        fprintf(stderr, "[PROBE] DMA_HEAP_IOCTL_ALLOC(len=%zu) failed: %s\n", len, strerror(errno));
        return false;
    }
    out->fd = static_cast<int>(data.fd);
    out->size = len;
    out->map = mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, out->fd, 0);
    if (out->map == MAP_FAILED)
    {
        fprintf(stderr, "[PROBE] mmap dmabuf(len=%zu) failed: %s\n", len, strerror(errno));
        close(out->fd);
        out->fd = -1;
        return false;
    }
    return true;
}

/** CPU-side cache maintenance on the dmabuf (dma-buf sync contract). */
static bool dmabuf_sync_end(int fd)
{
    struct dma_buf_sync sync = {};
    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    return ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
}

// ------------------------------------------------------------ page-aligned C —

static void *anon_pages(size_t len)
{
    return mmap(nullptr, (len + 4095) & ~size_t(4095), PROT_READ | PROT_WRITE,
                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
}

// ------------------------------------------------------------ pattern fill —

/** Deterministic NV12 pattern; the frame index salts every byte so consecutive
 *  frames are distinguishable (tear detection in the consistency loop). */
static void fill_nv12(uint8_t *y, uint8_t *uv, size_t w, size_t h, uint32_t frame)
{
    for (size_t r = 0; r < h; r++)
        for (size_t c = 0; c < w; c++)
            y[r * w + c] = static_cast<uint8_t>((r * 7 + c * 13 + frame * 29) & 0xFF);
    const size_t uv_len = w * h / 2;
    for (size_t i = 0; i < uv_len; i++)
        uv[i] = static_cast<uint8_t>((i * 11 + frame * 37) & 0xFF);
}

// ------------------------------------------------------------------- probe —

struct ProbeCtx
{
    std::shared_ptr<InferModel> model;
    ConfiguredInferModel configured;
    std::vector<std::string> in_names;
    std::vector<std::string> out_names;
    /* page-aligned output buffers, bound once and hashed after every run */
    struct OutBuf
    {
        void *p = nullptr;
        size_t size = 0;
    };
    std::vector<OutBuf> outs;
};

struct RunStat
{
    hailo_status st = HAILO_UNINITIALIZED;
    uint64_t bind_us = 0;
    uint64_t submit_us = 0;
    uint64_t wait_us = 0;
    std::vector<uint64_t> hashes; /* one FNV-1a per output tensor */
};

/**
 * One inference: bind the input via `bind` (path-specific), submit run_async,
 * wait for completion, hash every output buffer in ctx.outs.
 * Output buffers must already be bound on `b` (bind_outputs()).
 */
static RunStat run_once(ProbeCtx &ctx, Bindings &b,
                        const std::function<hailo_status()> &bind)
{
    RunStat rs;
    uint64_t t0 = now_us();
    rs.st = bind();
    rs.bind_us = now_us() - t0;
    if (rs.st != HAILO_SUCCESS)
        return rs;

    t0 = now_us();
    auto job = ctx.configured.run_async(b, [](const hailort::AsyncInferCompletionInfo &) {});
    rs.submit_us = now_us() - t0;
    if (!job)
    {
        rs.st = job.status();
        return rs;
    }
    t0 = now_us();
    rs.st = job->wait(std::chrono::milliseconds(10000));
    rs.wait_us = now_us() - t0;
    if (rs.st != HAILO_SUCCESS)
        return rs;

    for (const auto &o : ctx.outs)
        rs.hashes.push_back(fnv1a(o.p, o.size));
    return rs;
}

/** Bind page-aligned CPU buffers to every output edge. The manual requires the
 *  *allocated* output size to be PAGE_SIZE-aligned (memory corruption risk
 *  otherwise) — anon_pages() guarantees that. */
static bool bind_outputs(ProbeCtx &ctx, Bindings &b)
{
    for (size_t i = 0; i < ctx.out_names.size(); i++)
    {
        const auto &name = ctx.out_names[i];
        auto edge = b.output(name);
        if (!edge)
        {
            fprintf(stderr, "[PROBE] bindings.output(%s): st=%d\n", name.c_str(), (int)edge.status());
            return false;
        }
        const size_t fsz = ctx.model->output(name)->get_frame_size();
        ProbeCtx::OutBuf ob;
        ob.p = anon_pages(fsz);
        ob.size = fsz;
        if (ob.p == MAP_FAILED)
            return false;
        hailo_status st = edge->set_buffer(hailort::MemoryView(ob.p, fsz));
        if (st != HAILO_SUCCESS)
        {
            fprintf(stderr, "[PROBE] output set_buffer(%s): st=%d\n", name.c_str(), (int)st);
            return false;
        }
        ctx.outs.push_back(ob);
    }
    return true;
}

static const char *stname(hailo_status st)
{
    return hailo_get_status_message(st);
}

/** Aggregate a path over its runs; compare hashes against `baseline` when
 *  given. FAIL at first non-success status (bind or submit). */
static void report(const char *path, const std::vector<RunStat> &runs,
                   const std::vector<uint64_t> *baseline)
{
    if (runs.empty())
    {
        printf("[PROBE] path=%s verdict=SKIPPED\n", path);
        return;
    }
    const hailo_status st = runs.front().st;
    if (st != HAILO_SUCCESS)
    {
        printf("[PROBE] path=%s verdict=FAIL st=%d(%s) at=bind_or_submit\n",
               path, (int)st, stname(st));
        return;
    }
    uint64_t bind_sum = 0, wait_sum = 0;
    for (const auto &r : runs)
    {
        bind_sum += r.bind_us;
        wait_sum += r.wait_us;
    }
    bool match = true;
    if (baseline)
    {
        for (const auto &r : runs)
        {
            if (r.hashes.size() != baseline->size())
            {
                match = false;
                break;
            }
            for (size_t i = 0; i < r.hashes.size(); i++)
                if (r.hashes[i] != (*baseline)[i])
                {
                    match = false;
                    break;
                }
            if (!match)
                break;
        }
    }
    const size_t n = runs.size();
    printf("[PROBE] path=%s verdict=%s st=0 bind_avg_us=%llu wait_avg_us=%llu "
           "outputs=%zu match_vs_baseline=%s\n",
           path, match ? "PASS" : "MISMATCH", (unsigned long long)(bind_sum / n),
           (unsigned long long)(wait_sum / n), runs.front().hashes.size(),
           baseline ? (match ? "yes" : "NO") : "n/a");
}

int main(int argc, char **argv)
{
    const char *hef = nullptr;
    const char *group = "device0";
    const char *heap_override = nullptr;
    size_t width = 640, height = 384;
    int frames = 20;
    int consistency = 0;
    bool with_stream = false;
    bool skip_check = false;

    for (int i = 1; i < argc; i++)
    {
        auto next = [&](const char *what) -> const char *
        {
            if (i + 1 >= argc)
            {
                fprintf(stderr, "missing value for %s\n", what);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(argv[i], "--hef"))
            hef = next("--hef");
        else if (!strcmp(argv[i], "--group"))
            group = next("--group");
        else if (!strcmp(argv[i], "--heap"))
            heap_override = next("--heap");
        else if (!strcmp(argv[i], "--width"))
            width = strtoul(next("--width"), nullptr, 0);
        else if (!strcmp(argv[i], "--height"))
            height = strtoul(next("--height"), nullptr, 0);
        else if (!strcmp(argv[i], "--frames"))
            frames = atoi(next("--frames"));
        else if (!strcmp(argv[i], "--consistency"))
            consistency = atoi(next("--consistency"));
        else if (!strcmp(argv[i], "--with-stream"))
            with_stream = true;
        else if (!strcmp(argv[i], "--skip-cpu-check"))
            skip_check = true;
        else
        {
            fprintf(stderr,
                    "usage: npu-bind-probe --hef <model.hef> [--group device0] "
                    "[--heap /dev/dma_heap/system] [--width 640] [--height 384] "
                    "[--frames 20] [--consistency 1000] [--with-stream] [--skip-cpu-check]\n");
            return 2;
        }
    }
    if (!hef)
    {
        fprintf(stderr, "--hef is required\n");
        return 2;
    }

    const size_t y_len = width * height;
    const size_t uv_len = y_len / 2;
    const size_t nv12_len = y_len + uv_len;

    // ---- dmabufs -------------------------------------------------------------
    int heap_fd = open_dma_heap(heap_override);
    if (heap_fd < 0)
    {
        fprintf(stderr, "[PROBE] no dma_heap available (tried system/uncached/CMA)\n");
        return 1;
    }
    DmaBuf bufCombined, bufY, bufUV;
    if (!dmabuf_alloc(&bufCombined, heap_fd, nv12_len) ||
        !dmabuf_alloc(&bufY, heap_fd, y_len) ||
        !dmabuf_alloc(&bufUV, heap_fd, uv_len))
    {
        return 1;
    }
    close(heap_fd); /* dmabufs keep their own fds; heap fd not needed anymore */

    // ---- CPU mirror (baseline input + fill source) ---------------------------
    uint8_t *cpu = static_cast<uint8_t *>(anon_pages(nv12_len));
    if (cpu == MAP_FAILED)
    {
        fprintf(stderr, "[PROBE] anon_pages failed\n");
        return 1;
    }

    // ---- VDevice exactly like HAL --------------------------------------------
    hailo_vdevice_params_t vp = {};
    hailo_init_vdevice_params(&vp);
    vp.group_id = group;
    vp.multi_process_service = true;
    vp.scheduling_algorithm = HAILO_SCHEDULING_ALGORITHM_ROUND_ROBIN;
    auto vdev_exp = VDevice::create(vp);
    if (!vdev_exp)
    {
        fprintf(stderr, "[PROBE] VDevice::create(group=%s) st=%d(%s)\n",
                group, (int)vdev_exp.status(), stname(vdev_exp.status()));
        return 1;
    }
    auto vdev = vdev_exp.release();
    printf("[PROBE] vdevice group=%s multi_process_service=1 scheduler=ROUND_ROBIN\n", group);

    // ---- infer model ----------------------------------------------------------
    ProbeCtx ctx;
    auto model_exp = vdev->create_infer_model(hef);
    if (!model_exp)
    {
        fprintf(stderr, "[PROBE] create_infer_model st=%d(%s)\n",
                (int)model_exp.status(), stname(model_exp.status()));
        return 1;
    }
    ctx.model = model_exp.release();
    auto cfg_exp = ctx.model->configure();
    if (!cfg_exp)
    {
        fprintf(stderr, "[PROBE] configure st=%d(%s)\n",
                (int)cfg_exp.status(), stname(cfg_exp.status()));
        return 1;
    }
    ctx.configured = cfg_exp.release();

    for (const auto &n : ctx.model->get_input_names())
        ctx.in_names.push_back(n);
    for (const auto &n : ctx.model->get_output_names())
        ctx.out_names.push_back(n);
    if (ctx.in_names.size() != 1)
    {
        fprintf(stderr, "[PROBE] expected single-input model, got %zu\n", ctx.in_names.size());
        return 1;
    }
    const std::string &in_name = ctx.in_names[0];
    const size_t frame_size = ctx.model->input(in_name)->get_frame_size();
    if (frame_size != nv12_len)
    {
        /* Common trap: HEF geometry != --width/--height. Tell the operator the
         * exact expected value instead of failing byte_size later. */
        fprintf(stderr,
                "[PROBE] input frame_size=%zu but %zux%zu NV12 needs %zu — rerun with "
                "--width/--height matching the HEF input\n",
                frame_size, width, height, nv12_len);
        return 1;
    }
    printf("[PROBE] model inputs=%zu outputs=%zu frame_size=%zu\n",
           ctx.in_names.size(), ctx.out_names.size(), frame_size);

    // ---- create_bindings cost (ping-pong depth input) -------------------------
    {
        uint64_t t0 = now_us();
        const int N = 10;
        int ok = 0;
        for (int i = 0; i < N; i++)
        {
            auto bexp = ctx.configured.create_bindings();
            if (bexp)
                ok++;
        }
        printf("[PROBE] create_bindings avg_us=%llu ok=%d/%d\n",
               (unsigned long long)((now_us() - t0) / N), ok, N);
    }

    auto bindings_exp = ctx.configured.create_bindings();
    if (!bindings_exp)
    {
        fprintf(stderr, "[PROBE] create_bindings st=%d\n", (int)bindings_exp.status());
        return 1;
    }
    Bindings bMain = bindings_exp.release();
    if (!bind_outputs(ctx, bMain))
        return 1;

    // ---- baseline: CPU set_buffer ---------------------------------------------
    fill_nv12(cpu, cpu + y_len, width, height, 0);
    std::vector<RunStat> base_runs;
    for (int f = 0; f < frames; f++)
    {
        if (f > 0)
            fill_nv12(cpu, cpu + y_len, width, height, (uint32_t)f);
        base_runs.push_back(run_once(ctx, bMain, [&]
                                     {
                                         auto e = bMain.input(in_name);
                                         return e ? e->set_buffer(hailort::MemoryView(cpu, nv12_len))
                                                  : e.status();
                                     }));
        if (base_runs.back().st != HAILO_SUCCESS)
            break;
    }
    report("baseline", base_runs, nullptr);
    if (base_runs.empty() || base_runs.front().st != HAILO_SUCCESS)
    {
        fprintf(stderr, "[PROBE] baseline failed — nothing to compare against\n");
        return 1;
    }
    const std::vector<uint64_t> baseline = base_runs.back().hashes;

    // Mirror the exact same bytes into the dmabufs (combined + split pair).
    memcpy(bufCombined.map, cpu, nv12_len);
    memcpy(bufY.map, cpu, y_len);
    memcpy(bufUV.map, cpu + y_len, uv_len);
    dmabuf_sync_end(bufCombined.fd);
    dmabuf_sync_end(bufY.fd);
    dmabuf_sync_end(bufUV.fd);

    const std::vector<uint64_t> *check = skip_check ? nullptr : &baseline;

    // ---- path A control: set_pix_buffer USERPTR (validates the plane split) --
    {
        std::vector<RunStat> runs;
        for (int f = 0; f < frames; f++)
            runs.push_back(run_once(ctx, bMain, [&]
                                    {
                                        hailo_pix_buffer_t pb = {};
                                        pb.memory_type = HAILO_PIX_BUFFER_MEMORY_TYPE_USERPTR;
                                        pb.number_of_planes = 2;
                                        pb.planes[0].user_ptr = cpu;
                                        pb.planes[0].bytes_used = y_len;
                                        pb.planes[0].plane_size = y_len;
                                        pb.planes[1].user_ptr = cpu + y_len;
                                        pb.planes[1].bytes_used = uv_len;
                                        pb.planes[1].plane_size = uv_len;
                                        auto e = bMain.input(in_name);
                                        return e ? e->set_pix_buffer(pb) : e.status();
                                    }));
        report("pixbuf-userptr", runs, check);
    }

    // ---- path A: set_pix_buffer DMABUF (THE adjudication) ---------------------
    {
        std::vector<RunStat> runs;
        for (int f = 0; f < frames; f++)
            runs.push_back(run_once(ctx, bMain, [&]
                                    {
                                        hailo_pix_buffer_t pb = {};
                                        pb.memory_type = HAILO_PIX_BUFFER_MEMORY_TYPE_DMABUF;
                                        pb.number_of_planes = 2;
                                        pb.planes[0].fd = bufY.fd;
                                        pb.planes[0].bytes_used = y_len;
                                        pb.planes[0].plane_size = y_len;
                                        pb.planes[1].fd = bufUV.fd;
                                        pb.planes[1].bytes_used = uv_len;
                                        pb.planes[1].plane_size = uv_len;
                                        auto e = bMain.input(in_name);
                                        return e ? e->set_pix_buffer(pb) : e.status();
                                    }));
        report("pixbuf-dmabuf", runs, check);
    }

    // ---- path B1: set_dma_buffer, no prior dma_map ----------------------------
    {
        std::vector<RunStat> runs;
        for (int f = 0; f < frames; f++)
            runs.push_back(run_once(ctx, bMain, [&]
                                    {
                        auto e = bMain.input(in_name);
                        return e ? e->set_dma_buffer(hailo_dma_buffer_t{bufCombined.fd, nv12_len})
                                 : e.status();
                    }));
        report("dmabuf", runs, check);
    }

    // ---- path B2: set_dma_buffer after dma_map_dmabuf -------------------------
    {
        hailo_status ms = vdev->dma_map_dmabuf(bufCombined.fd, nv12_len,
                                               HAILO_DMA_BUFFER_DIRECTION_H2D);
        printf("[PROBE] dma_map_dmabuf st=%d(%s)\n", (int)ms, stname(ms));
        if (ms == HAILO_SUCCESS)
        {
            std::vector<RunStat> runs;
            for (int f = 0; f < frames; f++)
                runs.push_back(run_once(ctx, bMain, [&]
                                        {
                            auto e = bMain.input(in_name);
                            return e ? e->set_dma_buffer(hailo_dma_buffer_t{bufCombined.fd, nv12_len})
                                     : e.status();
                        }));
            report("dmabuf-mapped", runs, check);
        }
    }

    // ---- path C: low-level InputStream::write_async(dmabuf_fd) ----------------
    if (with_stream)
    {
        auto hef_exp = hailort::Hef::create(hef);
        if (!hef_exp)
            printf("[PROBE] path=stream verdict=FAIL st=%d(%s) at=Hef::create\n",
                   (int)hef_exp.status(), stname(hef_exp.status()));
        else
        {
            auto hef = hef_exp.release();
            auto ngs = vdev->configure(hef);
            if (!ngs || ngs->empty())
                printf("[PROBE] path=stream verdict=FAIL st=%d(%s) at=configure\n",
                       (int)ngs.status(), stname(ngs.status()));
            else
            {
                auto &ng = (*ngs)[0]; /* shared_ptr<ConfiguredNetworkGroup> */
                auto act = ng->activate();
                if (!act)
                    printf("[PROBE] path=stream verdict=FAIL st=%d(%s) at=activate\n",
                           (int)act.status(), stname(act.status()));
                else
                {
                    auto act_ng = act.release(); /* holds the group active */
                    (void)act_ng;
                    auto ins = ng->get_input_streams(); /* plain InputStreamRefVector */
                    auto outs = ng->get_output_streams();
                    if (ins.empty() || outs.empty())
                        printf("[PROBE] path=stream verdict=FAIL at=get_streams\n");
                    else
                    {
                        InputStream &in = ins[0].get();
                        const size_t hw_sz = in.get_info().hw_frame_size;
                        std::vector<ProbeCtx::OutBuf> souts;
                        bool ok = true;
                        for (auto &oref : outs)
                        {
                            const size_t osz = oref.get().get_info().hw_frame_size;
                            ProbeCtx::OutBuf ob{anon_pages(osz), osz};
                            if (ob.p == MAP_FAILED)
                            {
                                ok = false;
                                break;
                            }
                            souts.push_back(ob);
                        }
                        if (!ok)
                            printf("[PROBE] path=stream verdict=FAIL at=output_alloc\n");
                        else if (hw_sz != nv12_len)
                            printf("[PROBE] path=stream verdict=SKIPPED hw_frame_size=%zu != %zu\n",
                                   hw_sz, nv12_len);
                        else
                        {
                            uint64_t t0 = now_us();
                            hailo_status wst = HAILO_SUCCESS;
                            for (int f = 0; f < frames && wst == HAILO_SUCCESS; f++)
                            {
                                std::promise<hailo_status> done;
                                auto fut = done.get_future();
                                wst = in.write_async(bufCombined.fd, nv12_len,
                                                     [&done](const hailort::InputStream::CompletionInfo &ci)
                                                     { done.set_value(ci.status); });
                                if (wst != HAILO_SUCCESS)
                                    break;
                                wst = fut.get();
                                if (wst != HAILO_SUCCESS)
                                    break;
                                /* sync reads complete the round-trip */
                                for (size_t oi = 0; oi < outs.size(); oi++)
                                {
                                    wst = outs[oi].get().read(
                                        hailort::MemoryView(souts[oi].p, souts[oi].size));
                                    if (wst != HAILO_SUCCESS)
                                        break;
                                }
                            }
                            const uint64_t dt = now_us() - t0;
                            std::vector<uint64_t> h;
                            for (auto &ob : souts)
                                h.push_back(fnv1a(ob.p, ob.size));
                            bool match = !skip_check && h.size() == baseline.size();
                            if (match)
                                for (size_t i = 0; i < h.size(); i++)
                                    if (h[i] != baseline[i])
                                        match = false;
                            printf("[PROBE] path=stream verdict=%s st=%d(%s) frames_avg_us=%llu match_vs_baseline=%s\n",
                                   match ? "PASS" : (wst == HAILO_SUCCESS ? "MISMATCH" : "FAIL"),
                                   (int)wst, stname(wst), (unsigned long long)(dt / (size_t)frames),
                                   skip_check ? "n/a" : (match ? "yes" : "NO"));
                        }
                    }
                }
            }
        }
    }

    // ---- consistency loop (plan P0-7) ------------------------------------------
    if (consistency > 0)
    {
        /* Best direct path so far: set_dma_buffer (B2 mapping already done on
         * bufCombined). Each frame gets a fresh pattern written into the dmabuf
         * (CPU write + optional SYNC_END), the device path runs, then the CPU
         * baseline runs on a copy of the same bytes. Hash mismatch = torn or
         * stale read by the NPU. Alternating frames isolate whether the dma-buf
         * sync ioctl matters for coherence. */
        int mismatch_sync = 0, mismatch_nosync = 0;
        hailo_status cst = HAILO_SUCCESS;
        for (int f = 0; f < consistency && cst == HAILO_SUCCESS; f++)
        {
            fill_nv12(cpu, cpu + y_len, width, height, (uint32_t)(f + 1));
            memcpy(bufCombined.map, cpu, nv12_len);
            if (f & 1)
                dmabuf_sync_end(bufCombined.fd);
            RunStat dev = run_once(ctx, bMain, [&]
                                   {
                        auto e = bMain.input(in_name);
                        return e ? e->set_dma_buffer(hailo_dma_buffer_t{bufCombined.fd, nv12_len})
                                 : e.status();
                    });
            cst = dev.st;
            if (cst != HAILO_SUCCESS)
                break;
            RunStat cpur = run_once(ctx, bMain, [&]
                                    {
                        auto e = bMain.input(in_name);
                        return e ? e->set_buffer(hailort::MemoryView(cpu, nv12_len))
                                 : e.status();
                    });
            cst = cpur.st;
            if (cst != HAILO_SUCCESS)
                break;
            bool eq = dev.hashes.size() == cpur.hashes.size();
            for (size_t i = 0; eq && i < dev.hashes.size(); i++)
                eq = dev.hashes[i] == cpur.hashes[i];
            if (!eq)
                (f & 1) ? mismatch_sync++ : mismatch_nosync++;
        }
        printf("[PROBE] consistency frames=%d st=%d(%s) mismatch_with_sync=%d mismatch_without_sync=%d path=set_dma_buffer\n",
               consistency, (int)cst, stname(cst), mismatch_sync, mismatch_nosync);
    }

    // ---- two-bindings ping-pong (cross-bindings safety signal) -----------------
    {
        auto b2exp = ctx.configured.create_bindings();
        if (!b2exp)
            printf("[PROBE] pingpong verdict=FAIL at=create_bindings\n");
        else
        {
            Bindings b2 = b2exp.release();
            ProbeCtx ctx2; /* separate output buffers so writes don't alias */
            ctx2.model = ctx.model;
            ctx2.configured = ctx.configured;
            ctx2.in_names = ctx.in_names;
            ctx2.out_names = ctx.out_names;
            if (!bind_outputs(ctx2, b2))
                printf("[PROBE] pingpong verdict=FAIL at=bind_outputs\n");
            else
            {
                fill_nv12(cpu, cpu + y_len, width, height, 0x5A5);
                auto e1 = bMain.input(in_name);
                auto e2 = b2.input(in_name);
                hailo_status s1 = e1 ? e1->set_dma_buffer(hailo_dma_buffer_t{bufCombined.fd, nv12_len})
                                     : e1.status();
                memcpy(bufCombined.map, cpu, nv12_len);
                dmabuf_sync_end(bufCombined.fd);
                hailo_status s2 = e2 ? e2->set_buffer(hailort::MemoryView(cpu, nv12_len))
                                     : e2.status();
                if (s1 != HAILO_SUCCESS || s2 != HAILO_SUCCESS)
                    printf("[PROBE] pingpong verdict=FAIL bind1=%d(%s) bind2=%d(%s)\n",
                           (int)s1, stname(s1), (int)s2, stname(s2));
                else
                {
                    /* submit both, wait both — b1 reads the dmabuf, b2 reads CPU
                     * bytes with identical contents */
                    auto j1 = ctx.configured.run_async(bMain, [](const hailort::AsyncInferCompletionInfo &) {});
                    auto j2 = ctx.configured.run_async(b2, [](const hailort::AsyncInferCompletionInfo &) {});
                    if (!j1 || !j2)
                        printf("[PROBE] pingpong verdict=FAIL submit1=%d submit2=%d\n",
                               (int)j1.status(), (int)j2.status());
                    else
                    {
                        hailo_status w1 = j1->wait(std::chrono::milliseconds(10000));
                        hailo_status w2 = j2->wait(std::chrono::milliseconds(10000));
                        std::vector<uint64_t> h1, h2;
                        for (auto &o : ctx.outs)
                            h1.push_back(fnv1a(o.p, o.size));
                        for (auto &o : ctx2.outs)
                            h2.push_back(fnv1a(o.p, o.size));
                        bool eq = h1.size() == h2.size();
                        for (size_t i = 0; eq && i < h1.size(); i++)
                            eq = h1[i] == h2[i];
                        printf("[PROBE] pingpong verdict=%s wait1=%d wait2=%d outputs_equal=%s\n",
                               (w1 == HAILO_SUCCESS && w2 == HAILO_SUCCESS && eq) ? "PASS" : "FAIL",
                               (int)w1, (int)w2, eq ? "yes" : "NO");
                    }
                }
            }
        }
    }

    printf("[PROBE] done\n");
    return 0;
}
