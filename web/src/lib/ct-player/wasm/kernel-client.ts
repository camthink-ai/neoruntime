// @ts-nocheck
/**
 * WASM 内核客户端：经典 Worker 生命周期 + 消息协议 + 背压计数。
 */

import type { VideoCodec } from '../shared/types';
import { createLogger } from '../shared/log';

const log = createLogger('wasm');

/** 资产版本（与 kernel-worker.js 的 ASSET_VER 同步；升级即破缓存） */
const KERNEL_VERSION = '1.3.0';

export type KernelEvent =
  | { ev: 'loaded' }
  | { ev: 'configured'; codec: VideoCodec; threads?: number }
  | { ev: 'frame'; seq: number; width: number; height: number; yuv: ArrayBuffer; pts90k?: number }
  | { ev: 'decode-done'; seq: number; decodeMs?: number }
  | { ev: 'decode-error'; seq: number; ret: number }
  | { ev: 'error'; code: string; detail?: unknown }
  | { ev: 'load-error'; message: string }
  | { ev: 'log'; line: string };

export interface WasmKernelOptions {
  /** Base URL of kernel assets (ending in /); defaults to the bundled dist/wasm-kernel/ directory. */
  baseUrl?: string;
  /**
   * Decoder threads: 0 selects the single-threaded kernel; values above 1 use the multithreaded
   * kernel when cross-origin isolation and SharedArrayBuffer are available.
   * By default, isolated environments select 2–4 threads based on logical cores; others use 0.
   */
  threads?: number;
}

/** 多线程内核线程数上限（worker 侧 PTHREAD_POOL_SIZE=6 的安全余量） */
const MAX_THREADS = 4;

function autoThreads(): number {
  try {
    if (typeof SharedArrayBuffer === 'undefined') return 0;
    if (typeof crossOriginIsolated !== 'undefined' && !crossOriginIsolated) return 0;
    const cores = navigator.hardwareConcurrency || 4;
    return Math.max(2, Math.min(MAX_THREADS, cores - 1));
  } catch {
    return 0;
  }
}

export class WasmKernelClient {
  private worker: Worker | null = null;
  /** 已下发未完成的解码请求数（背压丢弃依据） */
  private outstanding = 0;
  private sentAt = new Map<number, number>();
  private completed = 0;
  private decodeTotalMs = 0;
  private roundTripTotalMs = 0;
  private readonly recentDecodeMs: number[] = [];
  private actualThreads: number | null = null;
  private peakOutstanding = 0;
  private loadFailed = false;
  /** 最近一次 configure 请求的线程数（0=单线程） */
  private threads = 0;

  constructor(
    private readonly opts: WasmKernelOptions,
    private readonly onEvent: (e: KernelEvent) => void
  ) {}

  /** 进行中的解码数。 */
  get inFlight(): number {
    return this.outstanding;
  }

  /** 最近一次 configure 请求的线程数（0=单线程内核）。 */
  get configuredThreads(): number {
    return this.threads;
  }

  get timings() {
    const sorted = [...this.recentDecodeMs].sort((a, b) => a - b);
    return { completed: this.completed, decodeMs: this.completed ? this.decodeTotalMs / this.completed : null,
      roundTripMs: this.completed ? this.roundTripTotalMs / this.completed : null, peakQueue: this.peakOutstanding,
      recentDecodeMs: sorted.length ? sorted.reduce((sum, value) => sum + value, 0) / sorted.length : null,
      recentDecodeP95Ms: sorted.length ? sorted[Math.ceil(sorted.length * 0.95) - 1] : null,
      threads: this.actualThreads };
  }

  private ensureWorker(): Worker | null {
    if (this.worker || this.loadFailed) return this.worker;
    try {
      // Vite/打包器静态分析要求 new URL 的 url 参数为字面量；
      // 资产版本（缓存击穿）改为运行时附加查询参数。
      const base = this.opts.baseUrl ?? new URL('../wasm-kernel/', import.meta.url).href;
      const workerUrl = new URL('kernel-worker.js', base);
      workerUrl.searchParams.set('v', KERNEL_VERSION);
      const worker = new Worker(workerUrl);
      worker.onmessage = (e: MessageEvent<KernelEvent>) => {
        const msg = e.data;
        if (!msg) return;
        if (msg.ev === 'configured') this.actualThreads = msg.threads ?? 0;
        if (msg.ev === 'decode-done') {
          this.outstanding = Math.max(0, this.outstanding - 1);
          const started = this.sentAt.get(msg.seq);
          if (started !== undefined && msg.decodeMs !== undefined) {
            this.completed++;
            this.decodeTotalMs += msg.decodeMs;
            this.recentDecodeMs.push(msg.decodeMs);
            if (this.recentDecodeMs.length > 60) this.recentDecodeMs.shift();
            this.roundTripTotalMs += performance.now() - started;
          }
          this.sentAt.delete(msg.seq);
        }
        this.onEvent(msg);
      };
      worker.onerror = (e) => {
        const where = ` @${e.lineno ?? 0}:${e.colno ?? 0}`;
        log.error('kernel worker error', `${e.message || 'error'}${where}`);
        this.loadFailed = true;
        this.onEvent({ ev: 'load-error', message: `${e.message || 'worker error'}${where}` });
      };
      this.worker = worker;
      worker.postMessage({ cmd: 'load', baseUrl: base });
      return worker;
    } catch (err) {
      log.error('kernel worker spawn failed', err);
      this.loadFailed = true;
      return null;
    }
  }

  configure(codec: VideoCodec, width?: number, height?: number, threads?: number): boolean {
    const worker = this.ensureWorker();
    if (!worker) return false;
    const clamp = (n: number) => Math.max(0, Math.min(MAX_THREADS, n));
    const wanted = threads !== undefined
      ? clamp(threads)
      : this.opts.threads !== undefined ? clamp(this.opts.threads) : autoThreads();
    this.threads = wanted;
    worker.postMessage({ cmd: 'configure', codec, width, height, threads: wanted });
    return true;
  }

  /** data 为规范 AnnexB 访问单元（会被 transfer，调用方不得复用）。 */
  decode(data: Uint8Array, seq: number, pts90k?: number): void {
    const worker = this.worker;
    if (!worker) return;
    this.outstanding += 1;
    this.sentAt.set(seq, performance.now());
    this.peakOutstanding = Math.max(this.peakOutstanding, this.outstanding);
    worker.postMessage({ cmd: 'decode', data: data.buffer, seq, pts90k }, [data.buffer]);
  }

  /**
   * 帧缓冲回收：渲染完成后把 frame 事件的 yuv 缓冲寄回 worker 复用，
   * 消除每帧 ~1.4MB（720p）的分配 churn（GC 停顿会误触背压丢弃）。
   * 回收的 buffer 归 worker 所有，调用方不得再引用。
   */
  recycle(yuv: ArrayBuffer): void {
    this.worker?.postMessage({ cmd: 'recycle', yuv }, [yuv]);
  }

  destroy(): void {
    if (!this.worker) return;
    this.worker.postMessage({ cmd: 'destroy' });
    // 给内核一点时间释放上下文，再终止
    const worker = this.worker;
    this.worker = null;
    setTimeout(() => {
      worker.onmessage = null;
      worker.onerror = null;
      worker.terminate();
    }, 250);
    this.outstanding = 0;
    this.sentAt.clear();
  }
}
