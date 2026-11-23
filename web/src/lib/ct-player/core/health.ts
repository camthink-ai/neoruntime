/**
 * HealthMonitor：运行时只读旁路采样（P3：不做决策，只发信号）。
 * - stall：传输在线但持续无帧（页面隐藏豁免）
 * - 错误风暴：10 秒窗口内 ≥3 次解码错误
 */

import { createLogger } from '../shared/log';

const log = createLogger('health');

export interface HealthMonitorOptions {
  /** 无帧判定窗口（ms），默认 5000 */
  stallMs?: number;
  now?: () => number;
  isHidden?: () => boolean;
  onStall?: () => void;
  onErrorBurst?: () => void;
  /** 采样间隔（ms），默认 1000 */
  tickMs?: number;
}

const ERROR_WINDOW_MS = 10 * 1000;
const ERROR_BURST_THRESHOLD = 3;

export class HealthMonitor {
  private readonly stallMs: number;
  private readonly now: () => number;
  private readonly isHidden: () => boolean;
  private readonly onStall?: () => void;
  private readonly onErrorBurst?: () => void;
  private readonly tickMs: number;
  private timer: ReturnType<typeof setInterval> | null = null;
  private lastFrameAt: number | null = null;
  private stalled = false;
  private errorTimestamps: number[] = [];
  private burstFired = false;
  /** 由 Player 注入：传输当前是否在线（未耗尽重试） */
  isOpen = false;

  constructor(opts: HealthMonitorOptions = {}) {
    this.stallMs = opts.stallMs ?? 5000;
    this.now = opts.now ?? (() => Date.now());
    this.isHidden = opts.isHidden ?? (() => typeof document !== 'undefined' && document.hidden);
    this.onStall = opts.onStall;
    this.onErrorBurst = opts.onErrorBurst;
    this.tickMs = opts.tickMs ?? 1000;
  }

  start(): void {
    this.stop();
    this.lastFrameAt = this.now();
    this.timer = setInterval(() => this.tick(), this.tickMs);
  }

  stop(): void {
    if (this.timer !== null) {
      clearInterval(this.timer);
      this.timer = null;
    }
  }

  reset(): void {
    this.lastFrameAt = this.now();
    this.stalled = false;
    this.errorTimestamps = [];
    this.burstFired = false;
  }

  markFrame(): void {
    this.lastFrameAt = this.now();
    if (this.stalled) this.stalled = false;
  }

  recordError(): void {
    const now = this.now();
    this.errorTimestamps.push(now);
    this.errorTimestamps = this.errorTimestamps.filter((t) => now - t < ERROR_WINDOW_MS);
    if (!this.burstFired && this.errorTimestamps.length >= ERROR_BURST_THRESHOLD) {
      this.burstFired = true;
      log.warn(`decode error burst: ${this.errorTimestamps.length} in ${ERROR_WINDOW_MS}ms`);
      this.onErrorBurst?.();
    }
  }

  /** 供测试与手动采样。 */
  tick(): void {
    if (!this.isOpen) return;
    if (this.isHidden()) return;
    const last = this.lastFrameAt ?? this.now();
    if (!this.stalled && this.now() - last >= this.stallMs) {
      this.stalled = true;
      log.warn(`stall detected: no frames for ${this.now() - last}ms`);
      this.onStall?.();
    }
    if (this.burstFired && this.errorTimestamps.length === 0) {
      this.burstFired = false;
    }
  }
}
