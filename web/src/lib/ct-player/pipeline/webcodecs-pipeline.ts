// @ts-nocheck
/**
 * WebCodecs 管道：H.264/H.265 VideoDecoder 直解 + Canvas 渲染。
 * 解码器突发输出按时间槽呈现，
 * 使用短时展示缓冲吸收抖动，队列过长时丢旧帧重新锚定。
 */

import type { EncodedFrame } from '../shared/types';
import type { Pipeline, PipelineContext, PipelineStats } from './types';
import { MediaClock } from '../media/clock';
import { h264CodecString } from '../media/sps-h264';
import { buildAvcConfigurationRecord } from '../media/fmp4/h264-muxer';
import { bytesEqual, frameToAvcc, isHevcVcl, isHevcRap } from '../media/nalu';
import { hevcCodecString, parseHevcSpsPtl } from '../media/fmp4/hevc-muxer';
import { concatBytes } from '../adapters/base';
import { createLogger } from '../shared/log';
import { DrawTiming } from './draw-timing';

const log = createLogger('webcodecs-pipeline');

/** 解码队列背压阈值：超过后丢弃非关键帧 */
const DECODE_QUEUE_CAP = 8;
/**
 * 解码输出时间戳回退容忍上限（µs）。设备按解码序推流（PTS 为显示序）时
 * 输出时间戳常态化回退（实测最大 ~333ms）——属显示序重排信号，按 dueMs
 * 有序插入即可；超过此值才视为时间轴重启（流切换/回绕）重新锚定。
 */
const REORDER_BACKTRACK_REANCHOR_US = 500_000;

export class WebCodecsPipeline implements Pipeline {
  readonly tech = 'webcodecs';

  private decoder: VideoDecoder | null = null;
  private decoderConfig: VideoDecoderConfig | null = null;
  private background = false;
  private readonly onVisibilityChange = () => {
    if (this.ctx.mode !== 'live' || typeof document === 'undefined') return;
    const hidden = document.hidden;
    if (hidden === this.background) return;
    this.background = hidden;
    this.drawTiming.pause();
    if (hidden && this.configuringHevc) {
      this.generation++;
      this.configuringHevc = false;
      this.pendingHevc = [];
      this.pendingHevcBytes = 0;
    }
    this.hasKey = false;
    this.drainPresentQueue();
    this.renderBaseUs = null;
    this.renderEpochMs = null;
    this.lastChunkTsUs = null;
    if (!this.decoder || !this.decoderConfig) return;
    try {
      // Reset discards stale decode work; reconfigure on return before waiting for an IDR.
      this.decoder.reset();
      if (!hidden) this.decoder.configure(this.decoderConfig);
    } catch (err) {
      this.ctx.onFatal('CONFIGURE_FAILED', err instanceof Error ? err.message : String(err));
    }
  };
  private ctx2d: CanvasRenderingContext2D | null = null;
  private canvas: HTMLCanvasElement | null = null;
  private sps: Uint8Array | null = null;
  private pps: Uint8Array | null = null;
  private vps: Uint8Array | null = null;
  private hevcCodec: string | null = null;
  private configuringHevc = false;
  private pendingHevc: EncodedFrame[] = [];
  private pendingHevcBytes = 0;
  private generation = 0;
  private hasKey = false;
  private clock = new MediaClock();
  private frameCount = 0;
  private readyReported = false;
  private presenting = true;
  private readonly drawTiming = new DrawTiming();
  private statsData: PipelineStats = { framesIn: 0, framesOut: 0, dropped: 0 };

  // 展示调度器状态
  private presentQueue: { frame: VideoFrame; dueMs: number }[] = [];
  private renderEpochMs: number | null = null;
  private renderBaseUs: number | null = null;
  private lastChunkTsUs: number | null = null;
  private presentTimer: number | null = null;

  constructor(private readonly ctx: PipelineContext) {}

  init(): void {
    if (typeof document !== 'undefined' && this.ctx.mode === 'live') {
      document.addEventListener('visibilitychange', this.onVisibilityChange);
      this.background = document.hidden;
    }
    const canvas = this.ctx.render.getCanvas('2d');
    if (!canvas) {
      this.ctx.onFatal('RENDER_INIT_FAILED', 'no canvas element');
      return;
    }
    this.ctx.render.useSurface('canvas');
    this.canvas = canvas;
    this.ctx2d = canvas.getContext('2d', { alpha: false });
    if (!this.ctx2d) {
      this.ctx.onFatal('RENDER_INIT_FAILED', 'canvas 2d context unavailable');
    }
  }

  submit(frame: EncodedFrame): void {
    this.statsData.framesIn += 1;
    if (frame.codec === 'h265') {
      if (!this.decoder) { this.collectHevc(frame); return; }
      for (const nal of frame.nalus) {
        if (nal.type < 32 || nal.type > 34) continue;
        const payload = frame.data.subarray(nal.offset + 4, nal.offset + nal.size);
        if (nal.type === 32) this.vps = payload.slice();
        else if (nal.type === 34) this.pps = payload.slice();
        else if (!this.sps || !bytesEqual(this.sps, payload)) {
          const ptl = parseHevcSpsPtl(payload);
          if (!ptl || hevcCodecString(ptl) !== this.hevcCodec) {
            this.ctx.onFatal('CODEC_MISMATCH', 'HEVC stream profile or level changed');
            return;
          }
          this.sps = payload.slice();
        }
      }
    }
    if (!this.decoder) {
      for (let i = 0; i < frame.nalus.length; i++) {
        const n = frame.nalus[i];
        const payload = frame.data.subarray(n.offset + 4, n.offset + n.size);
        if (n.type === 7 && !this.sps) this.sps = payload;
        else if (n.type === 8 && !this.pps) this.pps = payload;
      }
      if (this.sps && this.pps) {
        this.configureDecoder(this.sps, this.pps);
      } else {
        return; // 等参数集
      }
      if (!this.decoder) return; // configure 失败已走 onFatal
    } else if (frame.codec === 'h264' && this.sps && this.pps) {
      // 跟踪 in-band 参数集变化（旋转/其他客户端重配分辨率）：frameToAvcc
      // 会剥掉参数集，解码器若沿用旧 avcC 解新流必然花屏——检测到变化即
      // 重建 description 重新 configure，并等新几何的 IDR 再恢复解码。
      let sps: Uint8Array | null = null;
      let pps: Uint8Array | null = null;
      for (const nal of frame.nalus) {
        if (nal.type !== 7 && nal.type !== 8) continue;
        const payload = frame.data.subarray(nal.offset + 4, nal.offset + nal.size);
        if (nal.type === 7 && !bytesEqual(this.sps, payload)) sps = payload;
        else if (nal.type === 8 && !bytesEqual(this.pps, payload)) pps = payload;
      }
      if (sps || pps) this.reconfigureDecoder(sps ?? this.sps, pps ?? this.pps);
    }

    const hasSlice = frame.nalus.some((n) => frame.codec === 'h265' ? isHevcVcl(n.type) : n.type >= 1 && n.type <= 5);
    const keyframe = frame.codec === 'h265' ? frame.nalus.some(n => isHevcRap(n.type)) : frame.keyframe;
    if (!hasSlice) return;
    // Hidden live sessions deliberately skip decoding, not a decoder/network failure.
    if (this.background) { this.hasKey = false; return; }
    // 一旦丢掉参考帧，后续 delta 帧可能依赖它，必须等下一个关键帧。
    if (this.decoder.decodeQueueSize > DECODE_QUEUE_CAP && !keyframe) {
      this.hasKey = false;
      this.statsData.dropped += 1;
      return;
    }
    if (!this.hasKey) {
      if (!keyframe) {
        this.statsData.dropped += 1;
        return;
      }
      this.hasKey = true;
    }

    const timing = this.clock.next(frame.pts90k);
    // MediaClock 统一裁决：PTS 细粒度→真实相对 PTS；粗粒度/缺失→到达节奏合成轨
    // （秒级粗粒度 PTS 直接用会造成每秒一次 1s 的呈现跳变 = 成串出帧卡顿）
    const timestampUs = Math.round((timing.ptsRelative * 1000) / 90);

    // No HEVC description means Annex B. Include all parameter sets on every
    // random-access chunk even when the device sent them in separate messages.
    let data = frame.codec === 'h265' ? frame.data : frameToAvcc(frame, (t) => t >= 1 && t <= 5);
    if (frame.codec === 'h265' && keyframe && this.vps && this.sps && this.pps) {
      const start = new Uint8Array([0, 0, 0, 1]);
      for (const param of [this.pps, this.sps, this.vps]) data = concatBytes(concatBytes(start, param), data);
    }
    if (data.length === 0) return;

    try {
      this.decoder.decode(
        new EncodedVideoChunk({
          type: keyframe ? 'key' : 'delta',
          timestamp: timestampUs,
          data,
        })
      );
      this.frameCount += 1;
    } catch (err) {
      log.warn('decode chunk failed', err);
      this.ctx.onDecodeError?.();
      this.ctx.onFatal('DECODE_ERROR', err);
    }
  }

  private collectHevc(frame: EncodedFrame): void {
    for (const nal of frame.nalus) {
      const payload = frame.data.subarray(nal.offset + 4, nal.offset + nal.size).slice();
      if (nal.type === 32) this.vps = payload;
      else if (nal.type === 33) this.sps = payload;
      else if (nal.type === 34) this.pps = payload;
    }
    if (this.background) return; // Keep parameter sets, but do not queue hidden frames.
    if (this.configuringHevc || (this.vps && this.sps && this.pps)) {
      if (this.pendingHevc.length >= 120 || this.pendingHevcBytes + frame.data.byteLength > 16 * 1024 * 1024) {
        this.pendingHevc = [];
        this.pendingHevcBytes = 0;
        this.configuringHevc = false;
        this.generation++;
        this.ctx.onFatal('CONFIGURE_FAILED', 'HEVC configuration pending buffer limit exceeded');
        return;
      }
      this.pendingHevc.push(frame);
      this.pendingHevcBytes += frame.data.byteLength;
    }
    if (!this.configuringHevc && this.vps && this.sps && this.pps) {
      const ptl = parseHevcSpsPtl(this.sps);
      if (!ptl) { this.pendingHevc = []; this.pendingHevcBytes = 0; this.ctx.onFatal('CONFIGURE_FAILED', 'Invalid HEVC SPS profile'); return; }
      this.configuringHevc = true;
      void this.configureHevc(hevcCodecString(ptl), this.generation);
    }
  }

  private async configureHevc(codec: string, generation: number): Promise<void> {
    try {
      const config: VideoDecoderConfig = { codec, optimizeForLatency: true };
      const support = await VideoDecoder.isConfigSupported(config);
      if (generation !== this.generation) return;
      if (!support.supported) {
        this.configuringHevc = false;
        this.pendingHevc = [];
        this.pendingHevcBytes = 0;
        this.ctx.onFatal('CODEC_UNSUPPORTED', { codec, reason: 'HEVC WebCodecs configuration is not supported' });
        return;
      }
      const decoder = new VideoDecoder({
        output: vf => this.enqueueFrame(vf),
        error: err => { this.ctx.onDecodeError?.(); this.ctx.onFatal('DECODE_ERROR', err); },
      });
      decoder.configure(config);
      if (decoder.state !== 'configured') { decoder.close(); throw new Error(`decoder state: ${decoder.state}`); }
      this.decoder = decoder;
      this.decoderConfig = config;
      this.hevcCodec = codec;
      this.configuringHevc = false;
      const pending = this.pendingHevc;
      this.pendingHevc = [];
      this.pendingHevcBytes = 0;
      // Replay without double-counting input stats.
      for (const frame of pending) { this.statsData.framesIn--; this.submit(frame); }
      log.info(`decoder configured (${codec}, Annex B)`);
    } catch (err) {
      if (generation !== this.generation) return;
      this.configuringHevc = false;
      this.pendingHevc = [];
      this.pendingHevcBytes = 0;
      this.ctx.onFatal('CONFIGURE_FAILED', err instanceof Error ? err.message : String(err));
    }
  }

  private configureDecoder(sps: Uint8Array, pps: Uint8Array): void {
    try {
      const codec = h264CodecString(sps);
      const description = buildAvcConfigurationRecord(sps, pps);
      const decoder = new VideoDecoder({
        output: (vf) => this.enqueueFrame(vf),
        error: (err) => {
          log.error('decoder error', err);
          this.ctx.onDecodeError?.();
          this.ctx.onFatal('DECODE_ERROR', err);
        },
      });
      const config: VideoDecoderConfig = { codec, description, optimizeForLatency: true };
      decoder.configure(config);
      if (decoder.state !== 'configured') {
        this.ctx.onFatal('CONFIGURE_FAILED', `decoder state: ${decoder.state}`);
        return;
      }
      this.decoder = decoder;
      this.decoderConfig = config;
      log.info(`decoder configured (${codec})`);
    } catch (err) {
      log.error('configure failed', err);
      this.ctx.onFatal('CONFIGURE_FAILED', err);
    }
  }

  /** in-band SPS/PPS 变化：reset 丢弃旧几何在途输出，重建 avcC 后等下一个 IDR。 */
  private reconfigureDecoder(sps: Uint8Array, pps: Uint8Array): void {
    if (!this.decoder) return;
    try {
      const codec = h264CodecString(sps);
      const config: VideoDecoderConfig = {
        codec,
        description: buildAvcConfigurationRecord(sps, pps),
        optimizeForLatency: true,
      };
      this.decoder.reset();
      this.decoder.configure(config);
      this.decoderConfig = config;
      this.sps = sps;
      this.pps = pps;
      this.hasKey = false;
      log.info(`decoder reconfigured (${codec}, parameter sets changed)`);
    } catch (err) {
      log.error('reconfigure failed', err);
      this.ctx.onFatal('CONFIGURE_FAILED', err);
    }
  }

  // ---- 展示调度器（移植自底座 webcodecsPlayer） ----

  private enqueueFrame(frame: VideoFrame): void {
    if (this.background || !this.presenting || !this.ctx2d || !this.canvas) {
      frame.close();
      return;
    }
    const now = performance.now();
    const backtrackUs = this.lastChunkTsUs === null ? 0 : this.lastChunkTsUs - frame.timestamp;
    if (
      this.renderBaseUs === null ||
      this.renderEpochMs === null ||
      this.lastChunkTsUs === null ||
      backtrackUs > REORDER_BACKTRACK_REANCHOR_US
    ) {
      // 首帧 / 大回退（时间轴重启、90kHz 回绕）：重新锚定
      this.renderBaseUs = frame.timestamp;
      this.renderEpochMs = now + this.ctx.latency.jitterBufferMs;
      this.lastChunkTsUs = frame.timestamp;
      this.drainPresentQueue();
      this.presentQueue.push({ frame, dueMs: this.renderEpochMs });
      this.schedulePresent();
      return;
    }
    // 小幅回退 = 解码序流的显示重排。乱序流首帧常是组内最大 PTS：若锚点
    // 基点钉在它上面，后到的更早 PTS 帧 due 会落在过去而被过期折叠丢弃——
    // 把基点下移到更早 PTS 并按新映射重算队列（首帧显示于 now+jitter）。
    if (frame.timestamp < this.renderBaseUs) {
      this.renderBaseUs = frame.timestamp;
      this.renderEpochMs = now + this.ctx.latency.jitterBufferMs;
      for (const q of this.presentQueue) {
        q.dueMs = this.renderEpochMs + (q.frame.timestamp - this.renderBaseUs) / 1000;
      }
    }
    if (backtrackUs <= 0) this.lastChunkTsUs = frame.timestamp;

    let dueMs = this.renderEpochMs + (frame.timestamp - this.renderBaseUs) / 1000;
    // 长时间断流后重新建立缓冲，避免新到的帧全部立即呈现。
    if (this.presentQueue.length === 0 && dueMs < now - this.ctx.latency.jitterBufferMs) {
      this.renderBaseUs = frame.timestamp;
      this.renderEpochMs = now + this.ctx.latency.jitterBufferMs;
      dueMs = this.renderEpochMs;
    }
    let idx = this.presentQueue.length;
    while (idx > 0 && this.presentQueue[idx - 1].dueMs > dueMs) idx--;
    this.presentQueue.splice(idx, 0, { frame, dueMs });

    // 生产跑赢呈现时钟：丢旧帧并锚定到最新（保持贴 live edge）。
    // 队列按 dueMs 有序，newest 即按 PTS 最新（乱序流折叠后短暂乱序属可接受的追边代价）
    if (this.presentQueue.length > this.ctx.latency.maxPendingFrames) {
      const newest = this.presentQueue.pop()!;
      for (const item of this.presentQueue) item.frame.close();
      this.statsData.dropped += this.presentQueue.length;
      this.presentQueue = [newest];
      this.renderEpochMs = now + this.ctx.latency.jitterBufferMs;
      this.renderBaseUs = newest.frame.timestamp;
      newest.dueMs = this.renderEpochMs;
    }
    this.schedulePresent();
  }

  private schedulePresent(): void {
    if (this.presentTimer !== null) return;
    const next = this.presentQueue[0]?.dueMs;
    const delay = next === undefined ? 16 : Math.max(0, Math.min(16, next - performance.now()));
    this.presentTimer = window.setTimeout(() => {
      this.presentTimer = null;
      const now = performance.now();
      // 呈现所有到槽的帧；多帧同时过期时收敛到最新
      while (this.presentQueue.length > 0 && this.presentQueue[0].dueMs <= now) {
        const item = this.presentQueue.shift();
        if (!item) break;
        if (this.presentQueue.length > 0 && this.presentQueue[0].dueMs <= now) {
          item.frame.close();
          this.statsData.dropped += 1;
        } else {
          this.renderFrame(item.frame);
        }
      }
      if (this.presentQueue.length > 0) this.schedulePresent();
    }, delay);
  }

  private drainPresentQueue(): void {
    if (this.presentTimer !== null) {
      clearTimeout(this.presentTimer);
      this.presentTimer = null;
    }
    for (const item of this.presentQueue) item.frame.close();
    this.presentQueue = [];
  }

  private renderFrame(frame: VideoFrame): void {
    if (this.background || !this.presenting || !this.ctx2d || !this.canvas) {
      frame.close();
      return;
    }
    const cssW = this.canvas.clientWidth || this.canvas.parentElement?.clientWidth || frame.displayWidth;
    const cssH = this.canvas.clientHeight || this.canvas.parentElement?.clientHeight || frame.displayHeight;
    const dpr = window.devicePixelRatio || 1;
    const targetW = Math.max(1, Math.round(cssW * dpr));
    const targetH = Math.max(1, Math.round(cssH * dpr));
    if (this.canvas.width !== targetW || this.canvas.height !== targetH) {
      this.canvas.width = targetW;
      this.canvas.height = targetH;
    }

    const c = this.ctx2d;
    c.setTransform(dpr, 0, 0, dpr, 0, 0);
    c.fillStyle = 'black';
    c.fillRect(0, 0, cssW, cssH);
    const scale =
      this.ctx.objectFit === 'cover'
        ? Math.max(cssW / frame.displayWidth, cssH / frame.displayHeight)
        : Math.min(cssW / frame.displayWidth, cssH / frame.displayHeight);
    const drawW = frame.displayWidth * scale;
    const drawH = frame.displayHeight * scale;
    c.drawImage(frame, (cssW - drawW) / 2, (cssH - drawH) / 2, drawW, drawH);
    this.drawTiming.record(performance.now());
    frame.close();

    this.statsData.framesOut += 1;
    if (!this.readyReported) {
      this.readyReported = true;
      this.ctx.onReady();
    }
  }

  suspend(): void {
    this.presenting = false;
    this.drawTiming.pause();
    this.drainPresentQueue();
  }

  resume(): void {
    this.presenting = true;
    this.renderBaseUs = null;
    this.renderEpochMs = null;
    this.lastChunkTsUs = null;
  }

  stats(): PipelineStats {
    return { ...this.statsData, drawTiming: this.drawTiming.snapshot(performance.now()) };
  }

  latencyInfo(): { latencyMs: number | null; bufferedMs: number | null } {
    return { latencyMs: null, bufferedMs: null };
  }

  destroy(): void {
    if (typeof document !== 'undefined') document.removeEventListener('visibilitychange', this.onVisibilityChange);
    this.generation++;
    this.pendingHevc = [];
    this.pendingHevcBytes = 0;
    this.configuringHevc = false;
    this.presenting = false;
    this.drainPresentQueue();
    if (this.decoder) {
      try {
        if (this.decoder.state !== 'closed') this.decoder.close();
      } catch {
        /* ignore */
      }
      this.decoder = null;
    }
    this.decoderConfig = null;
    this.ctx2d = null;
    this.canvas = null;
    this.sps = null;
    this.pps = null;
    this.vps = null;
    this.hevcCodec = null;
    this.hasKey = false;
  }
}
