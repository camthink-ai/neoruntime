// @ts-nocheck
/**
 * MseSource：MediaSource/SourceBuffer 状态机 + 批量 append + 追帧 + 缓冲裁剪。
 * 移植自底座项目 media.ts 与 ne301 iOS 策略：
 * - 代数（generation）守卫防旧 sourceopen 回调污染新一代
 * - 批量 append：一次 updateend 窗口内合并积压片段，避免生产端跑赢消费端
 * - 独立定时器做 remove（remove 在 updateend 内会递归触发 updateend）
 * - 后台标签丢帧 + 回前台 seek live edge（Chrome 后台暂停 video-only 媒体的对策）
 */

import type { LatencyParams } from '../core/presets';
import type { CTErrorCode } from '../shared/errors';
import { createLogger } from '../shared/log';

const log = createLogger('mse');
const PLAYBACK_STALL_MS = 5000;
const RECENT_INPUT_MS = 2000;

interface MseSourceCallbacks {
  onFatal: (code: CTErrorCode, detail?: unknown) => void;
  onStarted: () => void;
  onDecodeError?: () => void;
}

type MediaSourceConstructor = new () => MediaSource;

export class MseSource {
  private mediaSource: MediaSource | null = null;
  private sourceBuffer: SourceBuffer | null = null;
  private mimeCodec = '';
  private frameBuffer: Uint8Array[] = [];
  /** 1=空闲可 append，0=updating 中 */
  private updateend = 1;
  private generation = 0;
  private started = false;
  private suspended = false;
  private cleanupTimer: ReturnType<typeof setInterval> | null = null;
  private stallTimer: ReturnType<typeof setInterval> | null = null;
  private lastInputAt = 0;
  private lastProgressAt = 0;
  private lastPlaybackTime = 0;
  private stallReported = false;
  private lastSeekTime = 0;
  private lastIosSeekTime = 0;
  private lastDecodeErrorRecovery = 0;
  private readonly video: HTMLVideoElement;
  private readonly params: LatencyParams;
  private readonly cb: MseSourceCallbacks;
  /** playback：全量缓冲、禁追帧/裁剪，后台持续灌数据 */
  private readonly playbackMode: boolean;
  private speed = 1;
  private readonly onVisibilityChange = () => {
    if (document.hidden || this.suspended || this.playbackMode) return;
    this.seekToLiveEdge();
    if (this.video.paused) this.video.play().catch(() => undefined);
  };
  private readonly onVideoError = (e: Event) => this.handleVideoError(e);

  constructor(
    video: HTMLVideoElement,
    params: LatencyParams,
    cb: MseSourceCallbacks,
    playbackMode = false
  ) {
    this.video = video;
    this.params = params;
    this.cb = cb;
    this.playbackMode = playbackMode;
    video.addEventListener('error', this.onVideoError);
    document.addEventListener('visibilitychange', this.onVisibilityChange);
  }

  /** 建 MediaSource 并挂到 video.src；SourceBuffer 在 sourceopen 后创建。 */
  open(mimeCodec: string): boolean {
    const MS = this.getMediaSourceCtor();
    if (!MS) {
      log.error('MediaSource not supported');
      return false;
    }
    this.closeBuffer(); // 新流切换：清理旧 MS 状态防解码器串流
    this.mimeCodec = mimeCodec;
    this.generation += 1;
    const gen = this.generation;
    this.started = false;

    try {
      const ms = new MS();
      this.mediaSource = ms;
      // 旧 src 的错误事件可能排队——换 src 前先摘除，新 src 设置后再挂回
      this.video.removeEventListener('error', this.onVideoError);
      if (this.video.src && this.video.src !== window.location.href) {
        try {
          URL.revokeObjectURL(this.video.src);
        } catch {
          /* ignore */
        }
      }
      this.video.src = URL.createObjectURL(ms);
      this.video.addEventListener('error', this.onVideoError);
      this.video.pause();

      ms.addEventListener('sourceopen', () => {
        if (this.mediaSource !== ms || this.generation !== gen) return;
        this.initSourceBuffer();
        this.flushQueue();
      });
    } catch (err) {
      log.error('open failed', err);
      return false;
    }
    return true;
  }

  private getMediaSourceCtor(): MediaSourceConstructor | null {
    const w = window as unknown as {
      ManagedMediaSource?: MediaSourceConstructor;
      MediaSource?: MediaSourceConstructor;
    };
    return w.ManagedMediaSource ?? w.MediaSource ?? null;
  }

  private initSourceBuffer(): void {
    if (!this.mediaSource || this.sourceBuffer) return;
    try {
      const sb = this.mediaSource.addSourceBuffer(this.mimeCodec);
      this.sourceBuffer = sb;
      if (sb.mode === 'segments') sb.mode = 'sequence';
      const gen = this.generation;
      sb.addEventListener('updateend', () => {
        if (this.sourceBuffer === sb && this.generation === gen) this.handleUpdateEnd();
      });
      sb.addEventListener('error', (e) => {
        if (this.sourceBuffer !== sb || this.generation !== gen) return;
        log.error('SourceBuffer error', e);
        this.cb.onFatal('DECODE_ERROR', { source: 'sourcebuffer' });
      });
      if (!this.playbackMode) {
        this.startCleanupTimer(); // 回放：全量保留，不裁剪
        this.startStallTimer();
      }
    } catch (err) {
      log.error('addSourceBuffer failed', err);
      this.cb.onFatal('CONFIGURE_FAILED', err);
    }
  }

  /** 追帧：updateend 窗口内执行（lag 基于最新 buffered.end）。 */
  private handleUpdateEnd(): void {
    this.updateend = 1;
    const sb = this.sourceBuffer;
    if (!sb || !this.mediaSource || this.mediaSource.readyState !== 'open') return;
    if (this.suspended || this.playbackMode) return; // 回放：无追帧
    try {
      const buffered = sb.buffered;
      if (buffered.length === 0) return;
      const lagSec = buffered.end(buffered.length - 1) - this.video.currentTime;
      const now = performance.now();

      if (this.params.usePlaybackRate) {
        if (lagSec * 1000 > this.params.hardSeekMs && now - this.lastSeekTime > this.params.seekCooldownMs) {
          const target = buffered.end(buffered.length - 1) - 0.1;
          if (target > this.video.currentTime) this.video.currentTime = target;
          this.video.playbackRate = 1.0;
          this.lastSeekTime = now;
        } else if (lagSec * 1000 > this.params.chaseStartMs) {
          this.video.playbackRate = this.params.chaseRate;
        } else if (lagSec * 1000 < this.params.startBufferMs) {
          this.video.playbackRate = 1.0;
        }
      } else {
        // iOS-WebKit：playbackRate 不可靠 → 低频 seek（ne301 策略，1s 冷却）
        if (
          lagSec * 1000 > this.params.chaseStartMs &&
          now - this.lastIosSeekTime > 1000
        ) {
          const target = buffered.end(buffered.length - 1) - 0.1;
          if (target > this.video.currentTime) this.video.currentTime = target;
          this.lastIosSeekTime = now;
        }
      }
    } catch (err) {
      log.warn('chase error', err);
    }
    this.flushQueue();
  }

  private flushQueue(): void {
    const sb = this.sourceBuffer;
    if (!sb || this.updateend !== 1) return;
    try {
      if (sb.updating) return;
    } catch {
      return; // SourceBuffer 已被移除
    }
    if (this.mediaSource?.readyState !== 'open' || this.video.error) return;
    if (this.frameBuffer.length === 0) return;

    // 队列上限：超限丢最老 30%（防后台/卡顿时积压无界增长）
    if (this.frameBuffer.length > this.params.maxPendingFrames) {
      const drop = Math.floor(this.params.maxPendingFrames * 0.3);
      this.frameBuffer.splice(0, drop);
    }

    const total = this.frameBuffer.reduce((s, b) => s + b.length, 0);
    const merged = new Uint8Array(total);
    let off = 0;
    for (const b of this.frameBuffer) {
      merged.set(b, off);
      off += b.length;
    }
    this.frameBuffer = [];
    try {
      sb.appendBuffer(merged);
      this.updateend = 0;
      if (this.video.paused) {
        this.video.play().then(() => {
          if (!this.started) {
            this.started = true;
            this.cb.onStarted();
          }
        }).catch(() => {
          // 浏览器拒绝 autoplay（Chrome 省电暂停）：下次 append 或回前台重试
        });
      }
    } catch (err) {
      log.error('appendBuffer failed', err);
      this.cb.onFatal('DECODE_ERROR', err);
    }
  }

  /** 独立定时器裁剪窗口（remove 不得在 updateend 内做，会递归）。 */
  private startCleanupTimer(): void {
    this.stopCleanupTimer();
    this.cleanupTimer = setInterval(() => {
      try {
        const sb = this.sourceBuffer;
        if (
          !sb ||
          sb.updating ||
          this.mediaSource?.readyState !== 'open' ||
          !this.video
        ) {
          return;
        }
        const buffered = sb.buffered;
        if (buffered.length === 0) return;
        const bufferStart = buffered.start(0);
        const bufferEnd = buffered.end(buffered.length - 1);
        const removeEnd = bufferEnd - this.params.bufferWindowSec;
        if (removeEnd > bufferStart && this.video.currentTime > bufferStart) {
          // 播放点前仅保留 0.5s（全 I 帧模式可随时从关键帧恢复）
          const safeEnd = Math.min(removeEnd, this.video.currentTime - 0.5);
          if (safeEnd > bufferStart) {
            sb.remove(bufferStart, safeEnd);
            const ms = this.mediaSource as MediaSource & {
              setLiveSeekableRange?: (s: number, e: number) => void;
            };
            ms.setLiveSeekableRange?.(safeEnd, bufferEnd);
          }
        }
      } catch {
        /* ignore */
      }
    }, 1000);
  }

  private stopCleanupTimer(): void {
    if (this.cleanupTimer !== null) {
      clearInterval(this.cleanupTimer);
      this.cleanupTimer = null;
    }
  }

  private startStallTimer(): void {
    if (this.stallTimer !== null) clearInterval(this.stallTimer);
    this.lastProgressAt = Date.now();
    this.lastPlaybackTime = this.video.currentTime;
    this.stallReported = false;
    this.stallTimer = setInterval(() => {
      const now = Date.now();
      const current = this.video.currentTime;
      if (!this.started || this.suspended || document.hidden || this.video.paused || this.video.seeking || this.video.ended || now - this.lastInputAt > RECENT_INPUT_MS) {
        this.lastProgressAt = now;
        this.lastPlaybackTime = current;
        return;
      }
      if (Math.abs(current - this.lastPlaybackTime) >= 0.01) {
        this.lastPlaybackTime = current;
        this.lastProgressAt = now;
        return;
      }
      if (!this.stallReported && now - this.lastProgressAt >= PLAYBACK_STALL_MS) {
        this.stallReported = true;
        this.cb.onFatal('DECODE_ERROR', { source: 'playback-stall' });
      }
    }, 1000);
  }

  append(data: Uint8Array): void {
    // 回放模式：后台也要持续灌数据（全量缓冲供 seek）；直播才做后台丢弃
    if (!this.playbackMode && (this.suspended || document.hidden) && this.mediaSource) return;
    this.lastInputAt = Date.now();
    if (this.frameBuffer.length > this.params.maxPendingFrames) {
      const drop = Math.floor(this.params.maxPendingFrames * 0.3);
      this.frameBuffer.splice(0, drop);
    }
    this.frameBuffer.push(data);
    this.flushQueue();
  }

  seekToLiveEdge(): void {
    const sb = this.sourceBuffer;
    if (!sb) return;
    try {
      const buffered = sb.buffered;
      if (buffered.length === 0) return;
      const lag = buffered.end(buffered.length - 1) - this.video.currentTime;
      if (lag > this.params.chaseStartMs / 1000) {
        this.video.currentTime = buffered.end(buffered.length - 1) - 0.1;
      }
    } catch {
      /* ignore */
    }
  }

  suspend(): void {
    this.suspended = true;
    this.video.pause();
  }

  resume(): void {
    this.suspended = false;
    if (!this.playbackMode) this.seekToLiveEdge();
    this.video.play().catch(() => undefined);
    this.flushQueue();
  }

  /** 回放：seek 到已缓冲内的任意时刻（秒） */
  seek(sec: number): void {
    const sb = this.sourceBuffer;
    if (!sb) return;
    try {
      const buffered = sb.buffered;
      for (let i = 0; i < buffered.length; i++) {
        if (sec >= buffered.start(i) && sec <= buffered.end(i)) {
          this.video.currentTime = sec;
          return;
        }
      }
      // 目标不在已缓冲区间：跳到最近边界（MSE 无未缓冲区的随机访问）
      const last = buffered.length - 1;
      if (last >= 0) {
        this.video.currentTime = sec < buffered.start(0) ? buffered.start(0) : buffered.end(last);
      }
    } catch {
      /* ignore */
    }
  }

  /** 回放：倍速（直播模式禁用，追帧策略独占 playbackRate） */
  setSpeed(rate: number): void {
    if (!this.playbackMode) return;
    const r = Math.min(4, Math.max(0.25, rate));
    this.speed = r;
    this.video.playbackRate = r;
  }

  timeline(): { durationMs: number | null; positionMs: number; ranges: Array<[number, number]> } {
    const sb = this.sourceBuffer;
    if (!sb) return { durationMs: null, positionMs: 0, ranges: [] };
    try {
      const b = sb.buffered;
      const ranges: Array<[number, number]> = [];
      for (let i = 0; i < b.length; i++) ranges.push([b.start(i), b.end(i)]);
      const durationMs = ranges.length ? ranges[ranges.length - 1][1] * 1000 : null;
      return { durationMs, positionMs: this.video.currentTime * 1000, ranges };
    } catch {
      return { durationMs: null, positionMs: 0, ranges: [] };
    }
  }

  latencyInfo(): { latencyMs: number | null; bufferedMs: number | null } {
    const sb = this.sourceBuffer;
    if (!sb) return { latencyMs: null, bufferedMs: null };
    try {
      const buffered = sb.buffered;
      if (buffered.length === 0) return { latencyMs: null, bufferedMs: null };
      const end = buffered.end(buffered.length - 1);
      const start = buffered.start(0);
      return {
        latencyMs: Math.max(0, (end - this.video.currentTime) * 1000),
        bufferedMs: (end - start) * 1000,
      };
    } catch {
      return { latencyMs: null, bufferedMs: null };
    }
  }

  private handleVideoError(e: Event): void {
    if (!this.mediaSource) return;
    const target = e.target as HTMLVideoElement;
    const err = target?.error;
    if (!err) return;
    if (err.code === err.MEDIA_ERR_ABORTED) return;
    if (err.code === err.MEDIA_ERR_DECODE) {
      const now = Date.now();
      if (now - this.lastDecodeErrorRecovery < 1000) {
        this.cb.onFatal('DECODE_ERROR', err);
        return;
      }
      this.lastDecodeErrorRecovery = now;
      this.cb.onDecodeError?.();
      this.cb.onFatal('DECODE_ERROR', err);
      return;
    }
    this.cb.onFatal('DECODE_ERROR', err);
  }

  /** MSE 错误后的同流重建（保留 SPS/PPS 由 pipeline 持有，可立即重建省 1-2s 黑屏）。 */
  reset(): void {
    this.closeBuffer();
    this.frameBuffer = [];
    this.updateend = 1;
  }

  private closeBuffer(): void {
    this.generation += 1;
    this.stopCleanupTimer();
    if (this.stallTimer !== null) clearInterval(this.stallTimer);
    this.stallTimer = null;
    this.started = false;
    this.lastInputAt = 0;
    if (this.sourceBuffer) {
      try {
        if (this.mediaSource && this.mediaSource.sourceBuffers.length > 0) {
          for (let i = 0; i < this.mediaSource.sourceBuffers.length; i++) {
            this.mediaSource.removeSourceBuffer(this.mediaSource.sourceBuffers[i]);
          }
        }
      } catch {
        /* ignore */
      }
      this.sourceBuffer = null;
    }
    if (this.mediaSource) {
      try {
        if (this.mediaSource.readyState === 'open') this.mediaSource.endOfStream();
      } catch {
        /* already closed */
      }
      this.mediaSource = null;
    }
  }

  destroy(): void {
    this.closeBuffer();
    this.video.removeEventListener('error', this.onVideoError);
    document.removeEventListener('visibilitychange', this.onVisibilityChange);
    this.video.playbackRate = 1.0;
    this.video.pause();
    if (this.video.src && this.video.src !== window.location.href) {
      try {
        URL.revokeObjectURL(this.video.src);
      } catch {
        /* ignore */
      }
      this.video.removeAttribute('src');
      this.video.load();
    }
    this.frameBuffer = [];
  }
}
