/**
 * Transport 门面：内置适配器走 Worker（收流+解析不占主线程），
 * 自定义适配器工厂 / Worker 不可用时回退主线程驱动。
 */

import type {
  AdapterRef,
  EncodedFrame,
  ReconnectPolicy,
  SourceAdapter,
  TransportStatus,
  WorkerCommand,
  WorkerEvent,
} from '../shared/types';
import type { Unsubscribe } from '../shared/events';
import { createBuiltinAdapter, isBuiltinAdapter } from '../adapters/registry';
import { createLogger } from '../shared/log';
import { WsDriver } from './driver';

const log = createLogger('transport');

type StatusHandler = (s: TransportStatus) => void;
type FrameHandler = (f: EncodedFrame) => void;

export interface WsTransportOptions {
  reconnect?: Partial<ReconnectPolicy>;
}

export class WsTransport {
  private worker: Worker | null = null;
  private driver: WsDriver | null = null;
  private statusHandlers = new Set<StatusHandler>();
  private frameHandlers = new Set<FrameHandler>();
  private stats = { bytes: 0, messages: 0 };
  private destroyed = false;

  onFrame(cb: FrameHandler): Unsubscribe {
    this.frameHandlers.add(cb);
    return () => this.frameHandlers.delete(cb);
  }

  onStatus(cb: StatusHandler): Unsubscribe {
    this.statusHandlers.add(cb);
    return () => this.statusHandlers.delete(cb);
  }

  getStats(): { bytes: number; messages: number } {
    return { ...this.stats };
  }

  /** 解析 AdapterRef → 内置描述符（可 Worker 化）或自定义工厂。 */
  private resolveAdapter(ref: AdapterRef): { descriptor: AdapterDescriptorLike } | { factory: () => Promise<SourceAdapter> } {
    if (typeof ref === 'string') return { descriptor: { name: ref, options: undefined } };
    if (typeof ref === 'function') return { factory: async () => {
      const mod = await ref();
      return 'default' in mod ? mod.default : mod;
    } };
    return { descriptor: { name: ref.name, options: ref.options } };
  }

  open(url: string, ref: AdapterRef = 'auto', opts: WsTransportOptions = {}): void {
    if (this.destroyed) return;
    const resolved = this.resolveAdapter(ref);
    const descriptor = 'descriptor' in resolved ? resolved.descriptor : null;
    const canWorker =
      descriptor !== null && isBuiltinAdapter(descriptor.name) && typeof Worker !== 'undefined';

    if (canWorker && descriptor) {
      try {
        this.startWorker(url, descriptor, opts);
        return;
      } catch (err) {
        log.warn('worker spawn failed, fallback to main thread', err);
      }
    }
    void this.startMain(url, resolved, opts);
  }

  private emitStatus(s: TransportStatus): void {
    for (const h of [...this.statusHandlers]) h(s);
  }

  private emitFrame(f: EncodedFrame): void {
    for (const h of [...this.frameHandlers]) h(f);
  }

  private startWorker(url: string, descriptor: AdapterDescriptorLike, opts: WsTransportOptions): void {
    const worker = new Worker(new URL('./ws-worker.js', import.meta.url), { type: 'module' });
    this.worker = worker;
    worker.onmessage = (e: MessageEvent<WorkerEvent>) => {
      const msg = e.data;
      if (!msg) return;
      if (msg.ev === 'status') {
        // Worker 存活期间发生未捕获异常（onerror）时转主线程兜底
        this.emitStatus(msg.status);
      } else if (msg.ev === 'frame') {
        this.emitFrame(msg.frame);
      } else if (msg.ev === 'stats') {
        this.stats = { bytes: msg.bytes, messages: msg.messages };
      }
    };
    worker.onerror = (err) => {
      log.warn('worker crashed, fallback to main thread', err.message || err);
      this.stopWorker();
      void this.startMain(url, { descriptor }, opts);
    };
    const cmd: WorkerCommand = {
      cmd: 'connect',
      url,
      adapter: { name: descriptor.name, options: descriptor.options },
      reconnect: opts.reconnect,
    };
    worker.postMessage(cmd);
  }

  private stopWorker(): void {
    if (!this.worker) return;
    this.worker.onmessage = null;
    this.worker.onerror = null;
    this.worker.terminate();
    this.worker = null;
  }

  private async startMain(
    url: string,
    resolved: { descriptor: AdapterDescriptorLike } | { factory: () => Promise<SourceAdapter> },
    opts: WsTransportOptions
  ): Promise<void> {
    let adapter: SourceAdapter | null = null;
    if ('factory' in resolved) {
      try {
        adapter = await resolved.factory();
      } catch (err) {
        this.emitStatus({ status: 'error', message: `adapter load failed: ${String(err)}` });
        return;
      }
    } else {
      adapter = createBuiltinAdapter({ name: resolved.descriptor.name, options: resolved.descriptor.options });
    }
    if (!adapter || this.destroyed) return;
    const driver = new WsDriver(
      adapter,
      {
        status: (s) => this.emitStatus(s),
        frame: (f) => this.emitFrame(f),
      },
      opts.reconnect
    );
    this.driver = driver;
    driver.start(url);
    const timer = setInterval(() => {
      if (this.driver !== driver || this.destroyed) {
        clearInterval(timer);
        return;
      }
      this.stats = driver.getStats();
    }, 1000);
  }

  close(): void {
    if (this.worker) {
      this.worker.postMessage({ cmd: 'disconnect' } satisfies WorkerCommand);
      // 给 worker 一点时间优雅关闭，然后强制终止
      setTimeout(() => this.stopWorker(), 250);
    }
    this.driver?.stop();
    this.driver = null;
  }

  destroy(): void {
    this.destroyed = true;
    this.close();
    this.statusHandlers.clear();
    this.frameHandlers.clear();
  }
}

interface AdapterDescriptorLike {
  name: string;
  options?: Record<string, unknown>;
}
