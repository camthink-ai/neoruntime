// @ts-nocheck
/**
 * WsDriver：WebSocket + 适配器 + 重连的驱动逻辑。
 * 同时运行于 Worker（ws-worker.ts）与主线程回退路径（ws.ts）。
 */

import type { EncodedFrame, ReconnectPolicy, SourceAdapter, TransportStatus } from '../shared/types';
import { ReconnectController, mergePolicy } from './reconnect';
import { createLogger } from '../shared/log';

const log = createLogger('ws');

export interface DriverEvents {
  status(s: TransportStatus): void;
  frame(f: EncodedFrame): void;
}

/** CONNECTING 卡死强关时限：握手不完成的 socket 会泄漏并占死后端连接。 */
const CONNECT_FALLBACK_MS = 4000;

export class WsDriver {
  private socket: WebSocket | null = null;
  private url: string | null = null;
  private stopped = true;
  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;
  private readonly reconnect: ReconnectController;
  private detectionTimer: ReturnType<typeof setTimeout> | null = null;
  private bytes = 0;
  private messages = 0;

  constructor(
    private readonly adapter: SourceAdapter,
    private readonly events: DriverEvents,
    policy?: Partial<ReconnectPolicy>
  ) {
    this.reconnect = new ReconnectController(mergePolicy(policy));
  }

  getStats(): { bytes: number; messages: number } {
    return { bytes: this.bytes, messages: this.messages };
  }

  start(url: string): void {
    this.stopped = false;
    this.url = url;
    this.connect();
  }

  stop(): void {
    this.stopped = true;
    this.reconnect.markManual();
    this.clearTimer();
    this.clearDetectionTimer();
    this.teardownSocket();
    this.adapter.reset();
  }

  private clearTimer(): void {
    if (this.reconnectTimer !== null) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
  }

  private clearDetectionTimer(): void {
    if (this.detectionTimer !== null) clearTimeout(this.detectionTimer);
    this.detectionTimer = null;
  }

  private failParsing(message: string): void {
    this.stop();
    this.events.status({ status: 'parse-error', message });
  }

  private teardownSocket(): void {
    const socket = this.socket;
    this.socket = null;
    if (!socket) return;
    try {
      if (socket.readyState === WebSocket.CONNECTING) {
        const forceTimer = setTimeout(() => {
          try {
            if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
              socket.close(1000, 'teardown-timeout');
            }
          } catch {
            /* ignore */
          }
        }, CONNECT_FALLBACK_MS);
        socket.addEventListener(
          'open',
          () => {
            clearTimeout(forceTimer);
            try {
              socket.close(1000, 'teardown');
            } catch {
              /* ignore */
            }
          },
          { once: true }
        );
        socket.addEventListener('close', () => clearTimeout(forceTimer), { once: true });
      } else if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CLOSING) {
        socket.close(1000, 'teardown');
      }
    } catch (err) {
      log.warn('teardown failed', err);
    }
  }

  private connect(): void {
    if (this.stopped || !this.url) return;
    this.teardownSocket();
    this.events.status({ status: 'connecting' });
    let socket: WebSocket;
    try {
      socket = new WebSocket(this.url);
    } catch (err) {
      this.events.status({ status: 'error', message: String(err) });
      this.scheduleReconnect();
      return;
    }
    this.socket = socket;
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      if (this.socket !== socket) return;
      this.reconnect.onOpen();
      this.events.status({ status: 'open' });
      if (this.adapter.isDetecting?.()) {
        this.clearDetectionTimer();
        this.detectionTimer = setTimeout(() => {
          if (this.socket === socket && this.adapter.isDetecting?.()) this.failParsing('Automatic stream format detection timed out; select the adapter manually');
        }, 10000);
      }
    };

    socket.onclose = (ev: CloseEvent) => {
      if (this.socket !== socket) return;
      this.socket = null;
      this.clearDetectionTimer();
      this.adapter.reset();
      this.handleClose(ev.code, ev.reason);
    };

    socket.onerror = () => {
      // close 事件随后必发，重连决策只在 onClose 做，避免叠加重试循环
    };

    socket.onmessage = (ev: MessageEvent) => {
      if (this.socket !== socket) return;
      const payload = ev.data;
      if (!(payload instanceof ArrayBuffer)) return;
      this.messages += 1;
      this.bytes += payload.byteLength;
      try {
        const frames = this.adapter.push(new Uint8Array(payload));
        if (this.adapter.isDetecting && !this.adapter.isDetecting()) this.clearDetectionTimer();
        for (const frame of frames) this.events.frame(frame);
      } catch (err) {
        this.failParsing(err instanceof Error ? err.message : String(err));
      }
    };
  }

  private handleClose(code: number, reason: string): void {
    const decision = this.reconnect.onClose(code, reason);
    switch (decision.action) {
      case 'report':
        this.events.status({ status: 'closed', code, reason });
        return;
      case 'superseded':
        this.events.status({ status: 'superseded', code, reason });
        return;
      case 'give-up':
        this.events.status({ status: 'retry-exhausted' });
        return;
      case 'reconnect':
        this.events.status({
          status: 'reconnecting',
          attempt: decision.attempt,
          delayMs: decision.delayMs,
        });
        this.clearTimer();
        this.reconnectTimer = setTimeout(() => {
          this.reconnectTimer = null;
          this.connect();
        }, decision.delayMs);
        return;
    }
  }

  private scheduleReconnect(): void {
    // 连接建立即抛异常（如 URL 非法）时走与关闭一致的重连路径
    this.handleClose(1006, 'connect-error');
  }
}
