# camera-daemon 问题排查报告（源码走读版）— 2026-11-05

> 排查设备：93.72（8GB）/ 93.220（4GB，root@192.168.93.x）
> 每个问题按 **复现 → 必现性 → 调用链走读（前因后果，逐步贴源码）→ 当前状态 → 恢复手段** 组织。
> 代码基准：当前工作区（两台设备部署的 `da91aff5`/`cf286b7b` 由此构建）。
> 走读中每一跳都标注了对应实测日志，链条可逐环核对。

---

# 问题 1：切 AIISP 到 Gen1 后黑屏（老问题 #14 的真实根因）

## 复现与必现性
前置：主码流 4K + NR≈100 + 辅流 h264@30（默认值即满足）。页面从 Gen3/Gen2 切 **Gen1**，切换成功后 10~20 秒（播放器重连触发主码流重配 4K）黑屏；或 Gen1+4K 长跑（93.72 实测 2h12m）自发黑屏。三路视频全停、音频正常。
Gen2/Gen3/Daylight 或主码流 ≤2.5K 时从未复现。

## 调用链走读

### ① 档位名到管线的映射——为什么只有 Gen1 危险
`webserver_medialib_config.json`（随 HAL 内嵌 bundle 部署）：
```json
{ "name": "AI_ISP_Gen1_Basic",
  "config_file": ".../lowlight/webserver_lowlight/webserver_lowlight_basic_profile.json" },
{ "name": "AI_ISP_Gen2_Basic",
  "config_file": ".../lowlight/webserver_lowlight_bayer/..." },
{ "name": "AI_ISP_Gen3_Basic",
  "config_file": ".../lowlight/webserver_ai_isp_gen3/..." }
```
Gen1 指向的 `webserver_lowlight`，其 `iq_settings.json` 的 denoise 节点（设备实测）：
```json
"denoise": {
  "enabled": true, "loopback-count": 1, "method": "HIGH_QUALITY",
  "network": {
    "network_path": ".../post_isp_denoise_678_h15h.hef",   // ← post-ISP，全分辨率 YUV 域
    "y_channel":         ".../input_layer1",
    "feedback_y_channel":".../input_layer3",                // ← 时域反馈：上一帧输出作为本帧输入
    "feedback_uv_channel":".../input_layer2", ... }
}
```
**因果**：Gen2/Gen3 的网络是 `pre_isp_*`（bayer 域，单平面反馈、数据量小、下游是定频 ISP 硬件）；Gen1 是 post-ISP 全分辨率 YUV 且 Y/UV 双平面都要反馈——4K 下单帧 12.4MB，每帧要移动读入+反馈+输出约 37MB。这是只有 Gen1 出事的**结构性原因**。

### ② 切换请求本身不会出事——出事在切换后的第二次重建
RPC 入口 `camera_control_service.cpp:412`（白名单校验后调用 `daemon_->switch_profile`）→ HAL `switch_profile`（`hailo15_media_impl.cpp:3525`）：
```cpp
HAL_LOG_INFO("... switch_profile: same stream-id layout as target — skip pre-set_profile disconnect");
... set_profile(...) OK ...
HAL_LOG_INFO("... switch_profile(%s) OK, ML pipeline_state=RUNNING ...");
```
实测（93.220 02:46:51-57）：切换完成、帧校验 OK、interrupt_ms=5323。**到这一步一切正常。**
10~20 秒后播放器重连，页面重放保存的码流偏好，`ReconfigurePipeline` 到达 → `reconfigure_pipeline`（`:7122`）：
```cpp
HAL_LOG_INFO("... reconfigure_pipeline: stream buffer layout change detected, full reinit");
```
主码流 2.5K→4K 是 buffer 布局变化，走全量销毁重建。**死锁发生在这次重建完成后的稳态**（93.220：reinit 完成后 6 秒；93.72：重建后 2 小时）。

### ③ 池只有 6 个 + vendor 的错误处理——死锁的最后一环
池大小的唯一来源是生成器（`gen_default_medialib_bundle.py`，配置编译进 HAL，daemon 启动时解包到 `/var/tmp/hal_medialib_default`）。修复提交的注释把根因写得很完整：
```python
# The MediaLibrary 1.13 default is only six 4K post-ISP denoise buffers. A
# transient producer/consumer clock skew can put all six in the downstream
# multi-resize queue; denoise treats the resulting transient acquire miss as a
# fatal frame error and permanently stops its GStreamer task. Twelve buffers
# cover the normal queueing window while adding about 75 MiB at 3840x2160 NV12.
DEFAULT_DENOISE_POOL_MAX_BUFFERS = 12
```
四个要素都有出处：**池默认 6**（vendor MediaLibrary 1.13 默认值）；**瞬时耗尽**（producer/consumer 时钟偏移把 6 个全推进下游 multi-resize 队列）；**按致命错误处理**（transient acquire miss → fatal frame error）；**永久停 task**（permanently stops its GStreamer task，无恢复路径）。
与实测对齐：`denoise.cpp:233 failed to acquire buffer for denoise output` 出现一次后，三路 dispatched 计数永久停止，pipeline 状态仍是 RUNNING——正是"task 退出但无错误上报"的表现。

### ④ 为什么黑得无声无息（旧版无自愈，新版见问题 6）
denoise 位于所有编码器上游，它停则三路全停；音频独立通路不受影响。旧版本没有任何组件检测"RUNNING 但无帧"。

## 当前状态与恢复
✅ 已部署两台：池 6→12（本节③的改动）+ 1Hz 巡检自愈（问题 6）。待 48h 浸泡（上次事故撑了 2h12m）。⚠️ 代码未提交，正式固件不含。旧版恢复手段：改主码流分辨率强制 reinit，或重启 daemon。

---

# 问题 3：快速来回切旋转后次码流黑屏（布局被残留竖向角度污染）

## 复现与必现性
20 秒内来回切旋转 2~4 次 + 刷新页面 → sub/third 黑屏、main 变竖向 4K；**旋转设回 0° 无效**，仅重启 camera-daemon 可恢复。依赖持久化残留竖向角度，连做 5~10 轮高概率撞出。

## 调用链走读（这是四个缺陷叠加的结果，逐个看）

### ① 旋转为什么会触发全量重建
`hailo15_media_impl.cpp:5325` 的分发逻辑——角度变了就全量重建：
```cpp
if (p.application_settings.rotation.effective_value() !=
    static_cast<rotation_angle_t>(cfg->rotation_angle)) {
    HAL_LOG_INFO("... rotation change (%d -> %d): using rotation_full_reinit", ...);
    return rotation_full_reinit(media_ctx, hm, priv, cfg);
}
```
提交 `2d2d1520`（09-28）的说明写了原因：在位旋转在 medialib v1.12.x 上每次开机重放 rot=180 都卡死（93.213/93.214），只能改走重建。**来回切 4 次 = 4 次全量重建，这是性能开销的来源，但还不是黑屏的原因。**

### ② 关键矛盾：两条路径对"竖向时分辨率要不要交换 W/H"的约定相反
**旋转路径（Pass B）明确不交换**——`patch_rotation_in_profile_files`（`:4836`）：
```cpp
/* Helper: patch application_settings content. Do NOT swap
 * application_input_streams resolutions — multi_resize's
 * get_output_resolution_by_index() already swaps dimensions when rotation
 * is 90/270. Swapping here too causes a double-swap that negates the
 * rotation. */
auto patch_app_settings = [&](json &app_obj) {
    app_obj["rotation"]["enabled"] = (angle != ROTATION_ANGLE_0);
    app_obj["rotation"]["angle"] = angle_str;      // ← 只写角度，不动分辨率
```
即：旋转路径的约定是"配置里保持横向数值，vendor 的 multi_resize 在运行时自己换"。

**但配置生成路径（Pass A）恰恰要交换**——`hailo15_generate_pipeline_config_json` 内（`:4634`）：
```cpp
HAL_LOG_INFO("... rotation_patch: %s before swap: %ux%u, to_portrait=%d", ...);
if (to_portrait) std::swap(w, h);        // ← encoder input_stream 的 W/H 被互换
inp["width"] = w;  inp["height"] = h;
```
**因果**：09:12:55 播放器重连触发 reconfigure → 走生成路径 → 只要生成时认为"当前是竖向"，encoder 尺寸就在配置层面被交换。实测 09:13:00 的产物：
```
reinit: encoder 'sink0' configured 2160x3840   ← 4K 转置，硬件恰好支持
reinit: encoder 'sink1' configured 720x1280    ← 竖向 720p
reinit: encoder 'sink2' configured 360x640     ← 竖向 360p
```

### ③ 生成路径为什么认为"当前是竖向"——残留角度从哪来
会话里测试者切的是 180°↔0°（`rotation change (2 -> 0)`、`(0 -> 2)`），**180° 不是竖向**（`to_portrait` 只对 90/270 为真）。但 reinit 完成路径会从缓存恢复图像配置：
```cpp
// camera_daemon.cpp:7329/7415/7515（三处 reinit 完成点都调用）
restore_image_config_if_cached();
```
```cpp
// camera_daemon.cpp:2155
void CameraDaemon::restore_image_config_if_cached() {
    ...
    int ret = media_ops->dynamic_change_image_config(media_ctx_, &last_image_config_);
    ...
    HAL_LOG_INFO("... Restored image config after reinit: rot=%d ...",
                 (int)last_image_config_.rotation_angle, ...);
```
`last_image_config_` 的写入点：`:907`（**开机重放持久化配置**时）、`:1596`、`:1934`。实测 09:13:00 该函数打出 `rot=3`（270°，竖向）——**一个从持久化带过来的旧竖向角度，而不是会话当前的 180°**。生成路径正是基于这个竖向状态做了 Pass A 的尺寸交换。两个状态源（会话值 vs 持久化缓存值）在同一轮重建里各说各话。

### ④ sub/third 喂帧被拒后，失败处理只有"日志限流"
`hailo15_ml_frontend_bridge.cpp:278`：
```cpp
media_library_return mr = enc_it->second->add_buffer(buf);
if (mr != MEDIA_LIBRARY_SUCCESS && p->feed_err_count[sid]++ < 5) {
    HAL_LOG_ERROR("... auto_feed add_buffer('%s') failed: %d", ...);
}
```
`feed_err_count[sid]++ < 5` **只约束打印条数**：前 5 帧打日志，之后每帧继续失败但完全静默——没有计数窗口、没有告警升级、没有恢复动作。实测 09:13:00 恰好 10 条（sub/third 各 5），此后无声。

### ⑤ 巡检为什么看不见
`camera_daemon.cpp:8908`（stall monitor 的流选择）：
```cpp
for (const auto& encoder : config_.encoders) {
    if (!encoder.enabled) continue;
    primary_config_name = encoder.stream_name;
    break;                      // ← 取第一个 enabled 就退出，只盯主流
}
```
main 活着（虽然方向不对）→ 巡检认为一切正常，sub/third 黑屏零检测。

### ⑥ 旋转设回 0°为什么救不回来
`SetTransform` 的重建只改角度字段（见②Pass B），不重放用户码流偏好；被交换过的分辨率留在 `stored_config_json` 与 /tmp 配置文件里（`app_settings_rot_*` 即 Pass B 的产物，`:4729/:4886`），后续重建继续沿用。没有任何路径做"角度归零时把分辨率归一化回横向"。

## 当前状态
❌ 未修复（P0）。修法与走读一一对应：③ 统一状态源（会话值权威、持久化仅在显式保存时写）；②/⑥ `align_resolutions` 增加横竖一致性校验（rot=0/180 全 W≥H、90/270 全 W≤H，非法 clamp）+ transform 重建后重放偏好；④ 失败升级（连败≥2s → ERROR + 触发 reinit）；⑤ 巡检遍历全部 enabled 流。恢复手段：重启 camera-daemon。

---

# 问题 4：改分辨率 / 编码 / 帧率 / 旋转，页面接口约 10 秒

## 复现与必现性
任意机器改任一项：分辨率/编码/帧率 ~9-10s，旋转 12-14s。**100% 必现**（设计代价，非偶发）。

## 调用链走读

### ① 以前有多快：39ms 的在位路径
初始版本对"分辨率/帧率-only"的修改走 `try_light_reconfigure`（`set_override_parameters` 原地改参）。这段历史保存在 `f0821413`（09-29）提交说明第 4 条里：
```
Light set_override path retired for RUNNING pipelines: a field-device core dump
showed queue src-pad tasks dying in pthread_mutex_lock on a freed shared object
after an in-place set_override_parameters() window (39ms) mutated encoder/pool
state while dataflow threads were live.
```
**因果**：数据流线程存活期间原地改 encoder/pool 状态 → vendor 队列线程锁住已释放对象（UAF）→ 现场 core dump。这个崩溃是真实的，退役决定本身没错。

### ② 现在的门槛：运行中管线永远全量重建
`hailo15_media_impl.cpp:7115`：
```cpp
const int light_rc = (same_stream_set && !pipeline_running)
                         ? try_light_reconfigure(...)
                         : HAL_OK; /* running pipeline or encoder set change — heavy path below */
```
页面改设置时管线必然在跑（`pipeline_running == true`）→ 恒走重建。且即使过了这道门，第二道门也把分辨率挡死——`reconfigure_is_fps_only`：
```cpp
    /* framerate-only changes stay eligible, and even those only on a
     * stopped pipeline (see reconfigure_pipeline). */
    return false; /* width or height changed */
```

### ③ 第二次收紧：codec 在位修改假成功（10-13 实测）
`encoder_inplace_ineffective_change` 的注释（`hailo15_media_impl.cpp:491` 附近）：
```cpp
 * Measured on medialib 1.13 (2026-10-13): a ReconfigureEncoder codec flip
 * h265->h264 took the in-place path (full_reinit_required=0) and the stream
 * stayed h265 until the next daemon restart — set_override_parameters does not
 * apply an encoder codec change.
```
在位返回成功但实际没生效——**当年快路径的真正死因是"改完不验证"**。

### ④ 重建为什么这么慢：5~6 秒里 3.5 秒在 vendor 黑盒
reinit 分段计时（实测稳定）：stop ~0.5-0.6s + destroy 0.1-1.2s + **`initialize()` 3.5s（内部零日志）** + connect ~0.5s。initialize 串行重做：解析 105 个配置文件、重载 denoise HEF 到 NN core、重新分配 ~450MB DMA 池——即使全都没变。

### ⑤ 页面为什么要等满：前端同步轮询
`web/src/pages/media/hooks/useMedia.ts:248`：
```ts
onSuccess: async (_response, variables) => {
    await refreshMediaStatusUntilStreamActive(queryClient, variables.stream_name);
    // MediaSettings keeps the full-reconfigure mutation pending while this
    // status refresh runs...
```
mutation 保持 pending 直到流恢复。文件顶部还留着过时注释 `"brief restart required, ~100ms"`——轻路径时代的遗物，与现状差两个数量级。HTTP 层预算被放宽到 30s（`media.go:810`，注释明说旋转 12-14s 会撞爆默认 10s）。

### ⑥ 旋转为什么翻倍
`SetTransform` 全量重建一次（问题 3 ①）+ 播放器重连重放偏好再触发一次 `ReconfigurePipeline` 全量重建（journal 中两次 reinit 间隔 10~20s 可证）。

## 当前状态
方案已评审（P1）：轻路径恢复资格 + **执行后读回验证**（profile 比对 dims/fps/codec + 首包 SPS 核对，2s 超时自动回退重建——补上③缺失的那道保险）；前端乐观返回 + 冻结末帧；`SetTransform` 带上偏好消除⑥的二次重建。目标：分辨率/帧率 <1s、旋转 <7s；vendor 侧诉求：initialize 支持池/HEF 复用。

---

# 问题 5：连续快速操作时进程崩溃（SIGABRT）

## 复现
低概率竞态：`ReconfigurePipeline` 与切档/旋转请求几乎同时到达。93.72 10:47:34 实发：`terminate called without an active exception`，CRASH REPORT 落在 `_ZN16EncodedPublisher5startEv+0x450`，调用链 `grpc sync 线程 → CameraDaemon::reconfigure_pipeline → EncodedPublisher::start`。

## 调用链走读
### ① 崩溃点的代码
`encoded_publisher.cpp:291-293`（`start()` 尾部）：
```cpp
accept_thread_    = std::thread(&EncodedPublisher::accept_loop, this);
dispatch_thread_  = std::thread(&EncodedPublisher::dispatch_loop, this);
keyframe_thread_  = std::thread(&EncodedPublisher::keyframe_dispatch_loop, this);
```
而 `stop()` 里对应的 join 在 `:336`：
```cpp
if (dispatch_thread_.joinable()) dispatch_thread_.join();
```
### ② C++ 语义层面的必然性
标准规定：对 **joinable** 的 `std::thread` 对象做移动赋值（`accept_thread_ = std::thread(...)` 即是）→ `std::terminate()`。触发条件：上一轮 `stop()` 的 join 尚未执行完（或根本没走到）时，`start()` 再次被调用。
### ③ 竞态窗口如何被打开
daemon 的 `reconfigure_pipeline` 在重建后重启消费者（`:3015` 附近 "Restarting pipeline after encoder reconfigure" → publisher stop/start 序列）；并发的切档/旋转走各自的重建路径也会 stop/start 同一个 publisher。两条路径的 stop→start 之间没有跨路径的互斥保护，start 抢在前一轮 join 完成前执行即命中②。
### ④ 证据吻合
崩溃栈符号（`EncodedPublisher::start+0x450`）、异常文本（terminate 无 active exception）、线程来源（grpc sync 线程执行 reconfigure）三者与①~③完全一致。

## 当前状态
❌ 未修复（P1）。修法：`start()` 前对三个线程成员强制 join/detach 收尾，或引入 stop→start 生命周期状态机/互斥；更彻底可换 `std::jthread`。副作用：崩溃断开所有连接、丢 AI 会话状态（systemd 5s 拉起仅是止损）。

---

# 问题 2：4GB 机型重刷后第三码流黑屏（CMA 连续内存耗尽）

## 复现与必现性
仅 **4GB 机型**（93.220）+ 重刷后首启窗口概率高；8GB（93.72）免疫。third 流黑（只有 metadata 包）、main 出 2MB 级异常大包。

## 调用链走读
### ① 内存峰值从哪来
重刷首启会重放持久化档位（AIISP 时建完整管线）+ 问题 1 修复的池 6→12 使 DMA 峰值 +75MB（来源即 `gen_default_medialib_bundle.py` 注释 "adding about 75 MiB at 3840x2160 NV12"）。
### ② 为什么分配会失败
视频帧 buffer 要求物理连续。设备 dtb 预留：`hailo_media_buf,cma` ~768MB（reusable）+ `linux,cma` ~512MB（reusable），合计等于 `CmaTotal` 1280MB；4GB 机型普通内存仅 ~2.6GB。重刷首启碎片化后连续 4MB/8MB 块凑不出：`dmesg: cma_alloc: linux,cma: alloc failed, req-size: 1013/2025 pages, ret: -12` ×7（1013/2025 页 ≈ 4K UV/Y 平面）。
### ③ 编码器的反应与内置诊断
拿不到 DMA buffer 的编码器继续发封装包但编码不出视频。检测代码（`encoded_publisher.cpp:815`）：
```cpp
HAL_LOG_ERROR(
    "EncodedPublisher [stream=%s]: ENCODER HEALTH ALARM — %lu packets "
    "published but only %lu keyframes (last IDR %ld ms ago). The encoder is "
    "emitting metadata-only packets and coding no video. Suspected DMA/CMA "
    "buffer starvation: check 'dmesg | grep cma_alloc' for ret: -12. "
    "A daemon restart usually does NOT fix this; a device reboot is "
    "the known remedy.", ...)
```
判定依据是 `pkts`（发包数）与 `kfs`（关键帧数）的背离 + `idr_gap_ms`；文案写明疗法是整机重启（buffer 泄漏在内核侧，进程重启无效）——与 93.220 实测处置一致。
### ④ 额外压力源
ISP 链路的 VB2 缓冲（10×8.3MB + 10×4.1MB，设备 dmabuf sysfs 实测 exporter 为 `videobuf2_dma_contig`）落在通用区，与内核 movable 页混住，进一步压缩回退空间。

## 当前状态
未根治，整机重启即愈。中期：`vm.min_free_kbytes`、reinit 前 `compact_memory`、`generate_config` 处按池尺寸×数量做 pre-flight 预算、vendor 把 VB2 迁专用区。**93.220 为内存类专用测试机。**

---

# 问题 6：黑屏自动恢复机制（已实机验证 ✅）

## 机制源码
常量（`camera_daemon.cpp`）：
```cpp
constexpr uint64_t kPipelineStallSnapshotMs = 10000;      // 10s 无包 → 快照
constexpr uint64_t kPipelineRecoveryFreshMs = 1500;       // 帧恢复判定阈值
constexpr auto kPipelineDiagnosticPollInterval = std::chrono::seconds(1);
constexpr uint64_t kPipelineRecoveryWindowSec = 10 * 60;  // 限流窗口
constexpr size_t  kPipelineRecoveryMaxAttempts = 3;       // 窗口内最多重启 3 次
```
主循环每秒采样（`:1061`）：
```cpp
while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (now >= next_diagnostic_poll) {
        poll_pipeline_stall_diagnostics();
        next_diagnostic_poll = now + kPipelineDiagnosticPollInterval;
    }
}
```
恢复动作（`:8968`）：
```cpp
if (!pipeline_stall_recovery_decided_ && raw_stalled) {
    pipeline_stall_recovery_decided_ = true;
    if (!reserve_pipeline_recovery_attempt()) return;  // 台账限次；IO 失败一律抑制（防重启风暴）
    HAL_LOG_ERROR("... PIPELINE_AUTO_RECOVERY restarting process ...");
    std::fflush(nullptr);
    // Avoid destructing a wedged vendor pipeline. camera-daemon.service has
    // Restart=always and recreates MediaLibrary (including its denoise pool)
    // from a clean process after RestartSec.
    std::_Exit(EXIT_FAILURE);
}
```
用 `_Exit` 不走析构的理由写在注释里：**不析构已卡死的 vendor 管线**（析构路径本身可能死锁），靠 systemd 从干净进程重建。

## 实机验证（11-05 18:19~18:23，93.72，da91aff5）
gdb non-stop 冻结数据流线程保持巡检线程运行：10s 快照完整（三路计数+62 线程）→ `reserved attempt=1/3` → `restarting process` → 进程退出 → systemd ~5s 拉起 → 出画。**停流到恢复 15~20s，限流台账生效**（`/run/aipc/camera-pipeline-recovery.state` 1 条记录）。
14:44 那次"只快照未恢复"发生在 14:40 过渡构建（二进制已被覆盖），风险随本次验证解除。

## 回归方法与测试坑
- 盯序列 `PIPELINE_STALL_SNAPSHOT → PIPELINE_AUTO_RECOVERY → PID 变化 → 15~20s 恢复`；只有快照无恢复记 P0
- 坑 1：`kill -STOP <tid>` 模拟不了死锁（SIGSTOP 停整个线程组，巡检线程一起被停）
- 坑 2：gdb non-stop 测试后必须 `kill -CONT <pid>` 清理

## 遗留加固（P1）
巡检调用 `frame_router_->ms_since_last_frame()` 需持 `frame_clock_mu_`，该锁同时被数据流回调线程持有——极端卡死下巡检自身可能阻塞（监控与被监控共享锁）。建议 try_lock 跳过本轮或无锁时间戳。

---

# 问题 7：重启后档位回退（开机重放失败）

断电/重启后 Gen3 变回 Daylight（`post-switch frame verify FAILED ... rolling back`）。AIISP 冷启动概率出现。
源码（`#101` 引入的预算与回退语义，`camera_daemon.cpp`）：
```cpp
// longer than the 5s warm-switch budget; replaying a persisted profile
// with that budget rolls back to the default profile on every boot.
constexpr uint64_t kBootReplayVerifyBudgetMs = 15000;
...
!verify_primary_stream_frames(10000)   // 开机重放给 10s 等帧，超时回退默认档
```
AIISP 冷启动出首帧偶发超过预算 → 设计内回退。老问题（#101）观测中。

---

# 日志取证速查

| 目的 | 命令（设备 root） |
|---|---|
| 流是否存活（每路约 2s 一条，不涨即死） | `journalctl -u camera-daemon -f \| grep dispatched` |
| 死锁类证据 | `grep -E "denoise\|PIPELINE_\|HEALTH ALARM\|add_buffer"` |
| 哪路先死 | `journalctl -u camera-daemon \| grep "stream=sub" \| tail -1` |
| CMA 失败 | `dmesg \| grep cma_alloc`（`ret: -12`） |
| 修复部署确认 | `/var/tmp/hal_medialib_default` 下 iq_settings.json 的 `denoise.pool-max-buffers` 应为 12 |

黑屏前兆：死锁前 ~40s `SLOW send/broadcast` 告警频率上升（93.72 13:02 实测）。
环境分工：93.72（8GB）测不出内存问题；93.220（4GB）是 CMA 基准机。

# 问题 ↔ 状态总表

| # | 问题 | 必现性 | 状态 |
|---|---|---|---|
| 1 | 切 Gen1 黑屏（denoise 池耗尽断链） | 条件凑齐基本必现 | ✅ 已修已部署（48h 浸泡待做），**代码未提交** |
| 2 | 4GB 重刷后流黑（CMA 耗尽） | 条件性 | ⚠️ 整机重启即愈，根治已列 |
| 3 | 旋转后次码流黑（布局污染，四缺陷叠加） | 高概率可撞出 | ❌ 未修（P0，修法对应走读①~⑥） |
| 4 | 改设置接口 10s（三次收紧叠加） | 100% | ❌ 方案已评审（P1） |
| 5 | 并发操作崩溃（joinable 线程赋值） | 低概率竞态 | ❌ 未修（P1，机制已定位） |
| 6 | 黑屏自动恢复 | — | ✅ 11-05 实机验证通过（15~20s，限流生效） |
| 7 | 重启后档位回退 | AIISP 冷启动概率 | 老问题（#101）观测中 |
