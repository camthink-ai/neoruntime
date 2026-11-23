/**
 * WASM 解码内核 Worker（经典 worker：Emscripten 胶水需要 importScripts）。
 *
 * 内核 ABI（当前随包分发的自建 FFmpeg 7.1 内核；构建管线见 docs/WASM-KERNEL.md）：
 *   FFMpegContextCreate(width, height, codetype 0=h264/3=h265)
 *   Decode(handle, ptr, size) → 同步触发全局
 *     thread_callback(yPtr, uPtr, vPtr, w, h, strideY, strideC, pts90k)   平面指针、stride 与输出画面 PTS
 *     thread_callback(dataPtr, w, h)                              旧内核：已打包连续 YUV
 *   FFMpegSetThreads(n)（仅 decoder-mt）→ Create 前 slice threading 线程数
 *   FFMpegContextDestroy(handle) / av_malloc / av_free
 *
 * 双内核：configure 带 threads>1 且 SharedArrayBuffer 可用（页面跨域隔离）时
 * 加载 decoder-mt（pthread + slice threading），否则回退 decoder（单线程）。
 *
 * 输出优化：worker 对每个平面只做一次行拷贝（源带 stride 对齐，目标紧致排列）
 * 直接写入 transferable 缓冲；旧实现为 glue 打包 + worker 再合并的双份全帧拷贝。
 */

/* eslint-disable no-undef */

// 资产版本（与 kernel-client.ts 的 KERNEL_VERSION 保持同步，升级时防旧缓存）
var ASSET_VER = '1.3.0';

/* pthread 委派：decoder-mt 胶水以 new Worker(<本脚本URL>, {name:'em-pthread'})
 * 再入本文件充当 pthread 池 worker——此时只加载同一份胶水，由胶水的
 * ENVIRONMENT_IS_PTHREAD 分支接管 onmessage；下方命令协议绝不能安装
 * （否则池 worker 装上本协议，emscripten 的池握手（cmd:1/3）永不完成 = 死锁）。 */
if (self.name === 'em-pthread') {
  // 崩溃诊断：池 worker 内的未捕获错误默认丢失堆栈，转发到 console（--enable-logging 可见）
  self.addEventListener('error', function (ev) {
    console.error('PTHREAD_CRASH', String((ev.error && ev.error.stack) || ev.message || ev));
  });
  importScripts(new URL('decoder-mt.js?v=' + ASSET_VER, self.location.href).href);
} else {
(function () {

var pendingSeq = 0;
var frameCallback = null;
var ready = false;
var pending = []; // runtime 异步初始化：postRun 前缓存的 configure/decode
var baseUrl = ''; // 'load' 时记录，首次 configure 按线程需求选内核
var kernelKind = ''; // '' 未加载 / 'st' / 'mt'
var threadsWanted = 0;

// YUV 缓冲回收池：主线程画完帧后把 transferable 缓冲寄回复用，
// 消除 ~1.4MB/帧（720p）的持续分配 → 主线程/worker 的 GC 压力是背压误触发的主因之一。
// 按字节数匹配（分辨率变化后旧缓冲自然淘汰），池深封顶防常驻内存。
var YUV_POOL_MAX = 8;
var yuvPool = [];

/** 逐行拷贝一个平面进合并缓冲（源行距 stride 含对齐填充，目标按紧致宽度排列）。 */
function copyPlaneRows(dst, dstOff, srcPtr, w, h, stride) {
  var heap = self.HEAPU8;
  for (var row = 0; row < h; row++) {
    dst.set(heap.subarray(srcPtr, srcPtr + w), dstOff);
    srcPtr += stride;
    dstOff += w;
  }
}

// 全局输出回调：由内核胶水内联调用（平面数据仅在本回调执行期间有效）。
// 两种 ABI 按位置区分：新内核 7 参平面指针 + stride；旧内核 3 参 (dataPtr, w, h)。
self.kernel_frame_out = function () {
  if (!frameCallback) return;
  var width, height, yLen, uvLen, merged;
  if (arguments.length >= 7) {
    width = arguments[3];
    height = arguments[4];
    yLen = width * height;
    uvLen = (width >> 1) * (height >> 1);
    merged = takeBuffer(yLen + uvLen * 2);
    copyPlaneRows(merged, 0, arguments[0], width, height, arguments[5]);
    copyPlaneRows(merged, yLen, arguments[1], width >> 1, height >> 1, arguments[6]);
    copyPlaneRows(merged, yLen + uvLen, arguments[2], width >> 1, height >> 1, arguments[6]);
  } else {
    // 旧 ABI：arguments = (dataPtr, width, height)，dataPtr 已是连续打包 YUV
    width = arguments[1];
    height = arguments[2];
    yLen = width * height;
    uvLen = (width >> 1) * (height >> 1);
    merged = takeBuffer(yLen + uvLen * 2);
    merged.set(HEAPU8.subarray(arguments[0], arguments[0] + yLen), 0);
    merged.set(HEAPU8.subarray(arguments[0] + yLen, arguments[0] + yLen + uvLen), yLen);
    merged.set(HEAPU8.subarray(arguments[0] + yLen + uvLen, arguments[0] + yLen + uvLen * 2), yLen + uvLen);
  }
  frameCallback(merged, width, height, pendingSeq, arguments.length >= 8 ? arguments[7] : -1);
};

/** 取一个输出缓冲：优先从回收池取同尺寸的，否则新分配。 */
function takeBuffer(total) {
  for (var i = 0; i < yuvPool.length; i++) {
    if (yuvPool[i].byteLength === total) return new Uint8Array(yuvPool.splice(i, 1)[0]);
  }
  return new Uint8Array(total);
}

// Module/HEAPU8 由胶水经 importScripts 挂到全局（self.Module / self.HEAPU8）；
// IIFE 内不得再声明同名 var 遮蔽，全部经 self.* 访问。
var api = null;
var handle = 0;
var codecType = -1;

function post(ev, transfer) {
  self.postMessage(ev, transfer || []);
}

function ensureApi() {
  if (api) return;
  var m = self.Module;
  api = {
    create: m.cwrap('FFMpegContextCreate', 'number', ['number', 'number', 'number']),
    destroy: m.cwrap('FFMpegContextDestroy', 'number', ['number']),
    decode: m.cwrap('Decode', 'number', ['number', 'number', 'number']),
    decodeTimed: typeof m._DecodeTimed === 'function' ? m.cwrap('DecodeTimed', 'number', ['number', 'number', 'number', 'number']) : null,
    malloc: m._av_malloc,
    free: m._av_free,
  };
  // 多线程内核导出的线程配置（ST 内核无此符号，不 cwrap）
  if (typeof m._FFMpegSetThreads === 'function') {
    api.setThreads = m.cwrap('FFMpegSetThreads', 'number', ['number']);
  }
  // 旧版胶水把 HEAPU8 挂在 Module 上；新版为全局 var（self.HEAPU8），此处同步兼容
  if (m.HEAPU8 && !self.HEAPU8) self.HEAPU8 = m.HEAPU8;
}

self.onmessage = function (e) {
  var msg = e.data || {};
  switch (msg.cmd) {
    case 'load': {
      baseUrl = msg.baseUrl || '';
      frameCallback = function (yuv, width, height, seq, pts) {
        post(
          { ev: 'frame', seq: seq, width: width, height: height, yuv: yuv.buffer, pts90k: pts >= 0 ? pts : undefined },
          [yuv.buffer]
        );
      };
      break;
    }
    case 'recycle': {
      // 主线程画完帧寄回缓冲（transferable），入池复用；尺寸不符的由 takeBuffer 自然淘汰
      if (msg.yuv && yuvPool.length < YUV_POOL_MAX) yuvPool.push(msg.yuv);
      break;
    }
    case 'configure': {
      // 首次 configure 决定内核形态：多线程内核（pthread + slice threading）仅在
      // 页面跨域隔离（SharedArrayBuffer 可用）且请求线程数 >1 时加载，否则回退 ST。
      threadsWanted = Math.max(0, Number(msg.threads) || 0);
      if (!kernelKind) loadKernel(threadsWanted > 1);
      if (!ready) {
        pending.push(msg);
        return;
      }
      handleReady(msg);
      break;
    }
    case 'decode':
    case 'destroy': {
      if (!ready) {
        // wasm 运行时未就绪：缓存（native 函数尚未导出可用）
        pending.push(msg);
        return;
      }
      handleReady(msg);
      break;
    }
    default:
      break;
  }
};

/** 按需加载内核并启动就绪轮询；SAB 不可用时自动回退单线程内核。 */
function loadKernel(wantsMt) {
  var useMt = wantsMt && typeof SharedArrayBuffer !== 'undefined';
  if (wantsMt && !useMt) {
    post({ ev: 'log', line: 'SharedArrayBuffer unavailable; falling back to the single-threaded kernel' });
  }
  // 主导 wasm 定位：locateFile 指向内核目录绝对地址（胶水自行流式加载，
  // 服务器需为 .wasm 提供 application/wasm 且 no-store）。
  try {
    self.Module = {
      locateFile: function (p) {
        return baseUrl + p;
      },
      printErr: function (t) {
        post({ ev: 'log', line: String(t) });
      },
    };
    var name = useMt ? 'decoder-mt.js' : 'decoder.js';
    importScripts(baseUrl + name + '?v=' + ASSET_VER);
    kernelKind = useMt ? 'mt' : 'st';
  } catch (err) {
    post({ ev: 'load-error', message: String((err && err.stack) || err) });
    return;
  }
  waitForRuntime();
}

/** 运行时就绪判定：导出函数可用（兼容同步/异步实例化）后冲刷 fetch 窗口期到达的全部命令。 */
function waitForRuntime() {
  var tries = 0;
  var timer = setInterval(function () {
    tries += 1;
    var m = self.Module;
    if (m && typeof m._av_malloc === 'function') {
      clearInterval(timer);
      ensureApi();
      ready = true;
      post({ ev: 'loaded' });
      var q = pending;
      pending = [];
      for (var i = 0; i < q.length; i++) self.onmessage({ data: q[i] });
    } else if (tries > 200) {
      // 10s 仍未就绪
      clearInterval(timer);
      post({ ev: 'load-error', message: 'runtime not ready after 10s' });
    }
  }, 50);
}

function handleReady(msg) {
  switch (msg.cmd) {
    case 'configure': {
      ensureApi();
      // 逆向结论（192.168.71.128 videoWorker.js）：Create(宽度, 高度, codetype)，
      // 前两参为输出缓冲尺寸；Decode 以同一"宽度"值作为上下文键。
      // codetype 语义：0=h264, 3=h265
      codecType = msg.codec === 'h265' ? 3 : 0;
      var w = msg.width || 1920;
      var h = msg.height || 1080;
      var threads = 0;
      if (kernelKind === 'mt' && threadsWanted > 1 && typeof self.Module._FFMpegSetThreads === 'function') {
        // 池深（PTHREAD_POOL_SIZE）是硬上界，超出会死锁
        threads = Math.min(threadsWanted, 6);
        api.setThreads(threads);
      }
      if (handle) api.destroy(handle);
      handle = w;
      var ret = api.create(w, h, codecType);
      if (ret < 0) {
        post({ ev: 'error', code: 'CONFIGURE_FAILED', detail: ret });
        return;
      }
      post({ ev: 'configured', codec: msg.codec, width: w, height: h, threads: threads });
      break;
    }
    case 'decode': {
      var decodeStarted = performance.now();
      if (!api || !handle) return;
      var data = new Uint8Array(msg.data);
      var size = data.byteLength;
      if (size <= 0) return;
      pendingSeq = msg.seq;
      // 帧头格式由 wasm 反汇编最终确认（Decode 在缓冲内扫描 magic 后按偏移取字段）：
      //   @0 magic 33 22 11 xx（xx 非 0）| @4 strm_type(1=VIDEO)
      //   @40 payload 大小 | @48 类型(1=video) | @52 payload 大小（C 取此字段 memcpy）
      //   @56 codetype(0=h264,3=h265) | @60 pts ms | @80 usec(64 位) | @88 起 AnnexB 负载
      var framed = new Uint8Array(88 + size);
      var dv = new DataView(framed.buffer);
      framed[0] = 0x33;
      framed[1] = 0x22;
      framed[2] = 0x11;
      framed[3] = 0x01; // magic（第 4 字节必须非零）
      dv.setInt32(4, 1, true); // strm_type = VIDEO
      dv.setInt32(40, size, true); // payload 长度（录制路径读取）
      dv.setInt32(44, handle, true); // ch = 上下文键（宽度值，与 Create 一致）
      dv.setInt32(48, 1, true); // type=video
      dv.setInt32(52, size, true); // payload 长度（C 解析取此字段）
      dv.setInt32(56, codecType, true);
      dv.setInt32(60, pendingSeq * 40, true); // pts ms
      dv.setUint32(80, pendingSeq * 40000, true); // usec 低位
      framed.set(data, 88);
      var ptr = api.malloc(framed.length);
      if (!ptr) {
        post({ ev: 'error', code: 'MALLOC_FAILED' });
        return;
      }
      var ret = -1;
      try {
        if (!self.HEAPU8) {
          post({ ev: 'error', code: 'MALLOC_FAILED', detail: 'HEAPU8 not ready' });
          return;
        }
        self.HEAPU8.set(framed, ptr);
        try {
          ret = api.decodeTimed ? api.decodeTimed(handle, ptr, framed.length, typeof msg.pts90k === 'number' ? msg.pts90k : -1) : api.decode(handle, ptr, framed.length);
        } catch (ex) {
          // 线程栈溢出等 wasm 异常在此捕获（worker 间 ErrorEvent 不携带堆栈）
          console.error('DECODE_THROW', String((ex && ex.stack) || ex).slice(0, 1800));
          post({ ev: 'decode-error', seq: msg.seq, ret: -999 });
          post({ ev: 'decode-done', seq: msg.seq });
          break;
        }
      } finally {
        api.free(ptr);
      }
      if (ret < 0) {
        post({ ev: 'decode-error', seq: msg.seq, ret: ret });
      }
      post({ ev: 'decode-done', seq: msg.seq, decodeMs: performance.now() - decodeStarted });
      break;
    }
    case 'destroy': {
      if (api && handle) {
        try {
          api.destroy(handle);
        } catch (err) {
          /* ignore */
        }
        handle = 0;
      }
      yuvPool = [];
      break;
    }
    default:
      break;
  }
}

})();
}
