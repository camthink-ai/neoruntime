// @ts-nocheck
/**
 * WASM 软解管道：EncodedFrame → WASM 内核（Worker 内 FFmpeg）→ YUV → WebGL 渲染。
 * 兜底定位：硬解不可用（如 Windows 无 HEVC 扩展的 H.265）时保住播放；
 * 背压策略：信用制下发（worker 内在途 ≤ MAX_INFLIGHT_DECODES 帧），本地等待队列
 * 超水位时整体清空贴 live edge，之后关键帧必发保持解码器同步。
 *
 * The decoder outputs pictures in presentation order. When output-picture PTS is
 * available, schedule presentation against the audio clock or a jitter buffer.
 * Never use the input request sequence as the output picture's timestamp.
 */

import type { EncodedFrame } from '../shared/types';
import type { Pipeline, PipelineContext, PipelineStats } from './types';
import { WasmKernelClient, type KernelEvent } from '../wasm/kernel-client';
import { WebGLYuvRenderer } from '../render/webgl-yuv';
import { parseH264SpsCodedSize } from '../media/sps-h264';
import { parseHevcSpsResolution } from '../media/fmp4/hevc-muxer';
import { createLogger } from '../shared/log';
import { DrawTiming } from './draw-timing';
import { AudioPresenter } from '../media/audio-presenter';
import { PacedPresenter } from '../media/paced-presenter';

const log = createLogger('wasm-pipeline');

// 解码信用：同一时刻最多压 MAX_INFLIGHT_DECODES 帧进 worker，其余在本地队列等待。
// 溢出丢弃只清本地未发送的帧——worker 侧的陈旧工作被信用上限封顶，恢复时
// 不必先消化整段积压才能轮到新关键帧。
const MAX_INFLIGHT_DECODES = 3;
// 本地等待队列水位：硬上限防爆内存；软阈值（深度+队首年龄）判定"持续追不上"。
// 实测 720p SIMD 内核 ~5ms/帧（约 5 倍实时余量），队列只在主线程卡顿（GC 等）
// 或推流突发时增长——此时应靠余量吞掉波动（毫秒级追平），而非丢帧等关键帧
// （GOP 内一次 trip = 数秒花屏）。故阈值按下界取值：
//   软年龄 1500ms：容忍 GC/节流级的主线程停顿；
//   硬上限 64 帧（≈2.5s @25fps）：真追不上时才贴 live edge 重同步。
const QUEUE_HARD_LIMIT = 64;
const QUEUE_SOFT_DEPTH = 8;
const QUEUE_HEAD_MAX_AGE_MS = 1500;
const RECOVERY_KEYFRAME_MAX_AGE_MS = 1000;

export class WasmPipeline implements Pipeline {
  readonly tech = 'wasm';

  private kernel: WasmKernelClient | null = null;
  private renderer: WebGLYuvRenderer | null = null;
  private presenter: AudioPresenter<{ yuv: ArrayBuffer; width: number; height: number }> | null = null;
  private pacedPresenter: PacedPresenter<{ yuv: ArrayBuffer; width: number; height: number }> | null = null;
  private configured = false;
  private hasKey = false;
  private suspended = false;
  private readyReported = false;
  private decodeErrors = 0;
  private configureStarted = 0;
  private initMs: number | null = null;
  private renderTotalMs = 0;
  private renderSamples = 0;
  private readonly drawTiming = new DrawTiming();
  private statsData: PipelineStats = { framesIn: 0, framesOut: 0, dropped: 0 };
  private dropReasons = { beforeConfiguration: 0, waitingForKeyframe: 0, queueOverflow: 0, suspended: 0, presentation: 0 };
  /** 等待下发 worker 的帧（信用制：仅在途 < MAX_INFLIGHT_DECODES 时发送） */
  private waiting: { frame: EncodedFrame; at: number }[] = [];
  private peakQueueLen = 0;
  /** SPS 解析出的尺寸（Create 需要真实宽高分配输出缓冲） */
  private dims: { width: number; height: number } | null = null;

  constructor(private readonly ctx: PipelineContext) {}

  init(): void {
    const canvas = this.ctx.render.getCanvas('webgl');
    if (!canvas) {
      this.ctx.onFatal('RENDER_INIT_FAILED', 'no canvas element');
      return;
    }
    this.ctx.render.useSurface('canvas');
    this.renderer = new WebGLYuvRenderer(canvas, this.ctx.objectFit);
    if (!this.renderer.available) {
      this.renderer.destroy();
      this.renderer = null;
      this.ctx.onFatal('RENDER_INIT_FAILED', 'WebGL unavailable');
    }
    if (this.renderer && this.ctx.audioClock) {
      this.presenter = new AudioPresenter({
        clock: this.ctx.audioClock.getPts90k,
        maxDriftMs: this.ctx.audioClock.maxDriftMs,
        present: frame => this.drawFrame(frame.yuv, frame.width, frame.height),
        discard: frame => { this.statsData.dropped++; this.dropReasons.presentation++; this.kernel?.recycle(frame.yuv); },
      });
    } else if (this.renderer && this.ctx.latency.jitterBufferMs > 0) {
      this.pacedPresenter = new PacedPresenter({
        jitterMs: this.ctx.latency.jitterBufferMs,
        maxPending: this.ctx.latency.maxPendingFrames,
        maxLeadMs: 1500,
        resyncOnOverflow: true,
        present: frame => this.drawFrame(frame.yuv, frame.width, frame.height),
        discard: frame => { this.statsData.dropped++; this.dropReasons.presentation++; this.kernel?.recycle(frame.yuv); },
      });
    }
  }

  submit(frame: EncodedFrame): void {
    this.statsData.framesIn += 1;
    // 尺寸未定：从 SPS 解析（Create 需要真实宽高）
    if (!this.dims) {
      const spsType = frame.codec === 'h264' ? 7 : 33;
      for (let i = 0; i < frame.nalus.length; i++) {
        if (frame.nalus[i].type === spsType) {
          const payload = frame.data.subarray(
            frame.nalus[i].offset + 4,
            frame.nalus[i].offset + frame.nalus[i].size
          );
          const d = frame.codec === 'h264' ? parseH264SpsCodedSize(payload) : parseHevcSpsResolution(payload);
          if (d) {
            this.dims = d;
            log.info(`stream dims ${d.width}x${d.height}`);
          }
          break;
        }
      }
    }
    if (!this.configured && this.dims) {
      const kernel = new WasmKernelClient(this.ctx.wasm ?? {}, (ev) => this.onKernelEvent(ev));
      this.configureStarted = performance.now();
      if (!kernel.configure(frame.codec, this.dims.width, this.dims.height)) {
        this.ctx.onFatal('WASM_LOAD_FAILED', 'kernel worker spawn failed');
        return;
      }
      this.kernel = kernel;
      this.configured = true;
      log.info(`kernel configuring for ${frame.codec} ${this.dims.width}x${this.dims.height}`);
    }
    if (!this.configured) {
      // 等参数集定尺寸
      this.dropReasons.beforeConfiguration += 1;
      this.statsData.dropped += 1;
      return;
    }

    if (!this.hasKey) {
      if (!frame.keyframe) {
        this.dropReasons.waitingForKeyframe += 1;
        this.statsData.dropped += 1;
        return;
      }
      this.hasKey = true;
    }

    if (this.suspended) {
      if (!frame.keyframe) {
        this.hasKey = false;
        this.dropReasons.suspended += 1;
        this.statsData.dropped += 1; // 挂起期间仅保留关键帧维持解码器同步
        return;
      }
    }

    const kernel = this.kernel;
    if (!kernel) return;
    this.waiting.push({ frame, at: performance.now() });
    this.dispatch();
  }

  /**
   * 有空闲信用时下发队首帧；等待队列超水位则整体清空贴 live edge。
   * 溢出只作用于未发送的本地帧；年龄只统计本地等待时间，正在解码的慢帧
   * （大 IDR 常见）不会触发误丢，只有队列真正积压老化才会。
   */
  private dispatch(): void {
    const kernel = this.kernel;
    if (!kernel) return;
    const head = this.waiting[0];
    if (this.waiting.length > QUEUE_HARD_LIMIT ||
        (this.waiting.length >= QUEUE_SOFT_DEPTH && head && performance.now() - head.at > QUEUE_HEAD_MAX_AGE_MS)) {
      // Resume at the newest recent keyframe already in the queue, if any.
      // Older in-flight frames remain before it in worker decode order.
      const now = performance.now();
      let keyIndex = -1;
      for (let i = this.waiting.length - 1; i > 0; i--) {
        if (this.waiting[i].frame.keyframe && now - this.waiting[i].at <= RECOVERY_KEYFRAME_MAX_AGE_MS) {
          keyIndex = i;
          break;
        }
      }
      const dropped = keyIndex >= 0 ? keyIndex : this.waiting.length;
      this.waiting = keyIndex >= 0 ? this.waiting.slice(keyIndex) : [];
      this.hasKey = keyIndex >= 0;
      this.dropReasons.queueOverflow += dropped;
      this.statsData.dropped += dropped;
    }
    while (this.waiting.length > 0 && kernel.inFlight < MAX_INFLIGHT_DECODES) {
      const next = this.waiting.shift()!;
      kernel.decode(next.frame.data, next.frame.seq, next.frame.pts90k);
    }
    this.peakQueueLen = Math.max(this.peakQueueLen, kernel.inFlight + this.waiting.length);
  }

  private onKernelEvent(ev: KernelEvent): void {
    switch (ev.ev) {
      case 'configured':
        this.initMs = performance.now() - this.configureStarted;
        log.info(`kernel configured (${ev.codec}${ev.threads ? `, slice x${ev.threads}` : ', single-threaded'}) init=${this.initMs}ms`);
        return;
      case 'decode-done':
        // 信用释放：立即补发等待队列的队首
        this.dispatch();
        return;
      case 'frame': {
        // The kernel returns display-order pictures with their own PTS when available.
        if (!this.suspended && this.renderer) {
          if (this.presenter && ev.pts90k !== undefined) this.presenter.push({ yuv: ev.yuv, width: ev.width, height: ev.height }, ev.pts90k);
          else if (this.pacedPresenter && ev.pts90k !== undefined) this.pacedPresenter.push([{ pts: ev.pts90k, item: { yuv: ev.yuv, width: ev.width, height: ev.height } }]);
          else { this.presenter?.untimed(); this.pacedPresenter?.clear(); this.drawFrame(ev.yuv, ev.width, ev.height); }
        } else {
          // 缓冲寄回 worker 复用（挂起未渲染的帧也回收），消除逐帧大分配的 GC 停顿
          this.kernel?.recycle(ev.yuv);
        }
        return;
      }
      case 'decode-error':
        // ffmpeg 对个别坏包返回错误是正常容忍（跳帧继续）：
        // 只有持续报错且始终无输出才判定致命，不参与全局风暴计数
        this.decodeErrors += 1;
        if (this.decodeErrors > 60 && this.statsData.framesOut === 0) {
          this.ctx.onFatal('DECODE_ERROR', { decodeErrors: this.decodeErrors, framesOut: 0 });
        }
        return;
      case 'log':
        log.info(ev.line);
        return;
      case 'error':
      case 'load-error':
        log.error('kernel fatal', ev);
        this.ctx.onFatal('WASM_LOAD_FAILED', ev);
        return;
      default:
        return;
    }
  }

  /** 渲染一帧并回收缓冲。 */
  private drawFrame(yuv: ArrayBuffer, width: number, height: number): void {
    if (this.renderer) {
      const started = performance.now();
      this.renderer.draw(new Uint8Array(yuv), width, height);
      this.drawTiming.record(performance.now());
      this.renderTotalMs += performance.now() - started;
      this.renderSamples++;
      this.statsData.framesOut += 1;
      this.statsData.videoSize = { width, height };
      if (!this.readyReported) {
        this.readyReported = true;
        this.ctx.onReady();
      }
    }
    // 缓冲寄回 worker 复用，消除逐帧大分配的 GC 停顿
    this.kernel?.recycle(yuv);
  }

  setObjectFit(fit: 'contain' | 'cover'): void {
    this.renderer?.setObjectFit(fit);
  }

  suspend(): void {
    this.suspended = true;
    this.presenter?.clear();
    this.pacedPresenter?.clear();
    this.drawTiming.pause();
  }

  resume(): void {
    this.suspended = false;
  }

  stats(): PipelineStats {
    const timings = this.kernel?.timings;
    return { ...this.statsData, ...(this.presenter ? { avSync: this.presenter.snapshot() } : {}), ...(this.pacedPresenter ? { wasmPresentation: this.pacedPresenter.diagnostics } : {}), drawTiming: this.drawTiming.snapshot(performance.now()), decodeErrors: this.decodeErrors, wasmDrops: { ...this.dropReasons }, wasmPerf: {
      decodeMs: timings?.decodeMs ?? null, roundTripMs: timings?.roundTripMs ?? null,
      recentDecodeMs: timings?.recentDecodeMs ?? null, recentDecodeP95Ms: timings?.recentDecodeP95Ms ?? null, threads: timings?.threads ?? null,
      renderMs: this.renderSamples ? this.renderTotalMs / this.renderSamples : null,
      queue: (this.kernel?.inFlight ?? 0) + this.waiting.length,
      peakQueue: Math.max(timings?.peakQueue ?? 0, this.peakQueueLen),
      samples: timings?.completed ?? 0, initMs: this.initMs,
    } };
  }

  latencyInfo(): { latencyMs: number | null; bufferedMs: number | null } {
    return { latencyMs: null, bufferedMs: null };
  }

  destroy(): void {
    this.presenter?.clear();
    this.presenter = null;
    this.pacedPresenter?.clear();
    this.pacedPresenter = null;
    this.kernel?.destroy();
    this.kernel = null;
    this.renderer?.destroy();
    this.renderer = null;
    this.configured = false;
    this.hasKey = false;
    this.waiting = [];
  }
}
