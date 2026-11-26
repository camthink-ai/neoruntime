// @ts-nocheck
/**
 * 传输 Worker：WebSocket 收流 + 内置适配器解析，帧以 transferable 零拷贝回传主线程。
 * 协议见 shared/types.ts 的 WorkerCommand / WorkerEvent。
 */

import { createBuiltinAdapter } from '../adapters/registry';
import type {
  AdapterDescriptor,
  EncodedFrame,
  TransportStatus,
  WorkerCommand,
  WorkerEvent,
} from '../shared/types';
import { WsDriver } from './driver';

const ctx = self as unknown as {
  postMessage(msg: WorkerEvent, transfer?: Transferable[]): void;
  onmessage: ((e: MessageEvent<WorkerCommand>) => void) | null;
};

let driver: WsDriver | null = null;
let statsTimer: ReturnType<typeof setInterval> | null = null;

function postEvent(ev: WorkerEvent, transfer?: Transferable[]): void {
  ctx.postMessage(ev, transfer);
}

function postStatus(status: TransportStatus): void {
  postEvent({ ev: 'status', status });
}

ctx.onmessage = (e: MessageEvent<WorkerCommand>) => {
  const msg = e.data;
  if (!msg) return;
  switch (msg.cmd) {
    case 'connect': {
      teardown();
      const adapter = createBuiltinAdapter(msg.adapter);
      if (!adapter) {
        postStatus({ status: 'error', message: `unknown adapter: ${msg.adapter.name}` });
        return;
      }
      driver = new WsDriver(adapter, {
        status: postStatus,
        frame: (frame: EncodedFrame) => {
          postEvent({ ev: 'frame', frame }, [frame.data.buffer as ArrayBuffer]);
        },
      }, msg.reconnect);
      driver.start(msg.url);
      statsTimer = setInterval(() => {
        if (!driver) return;
        const s = driver.getStats();
        postEvent({ ev: 'stats', bytes: s.bytes, messages: s.messages });
      }, 1000);
      break;
    }
    case 'disconnect': {
      teardown();
      postStatus({ status: 'closed', code: 1000, reason: 'client-disconnect' });
      break;
    }
    default:
      break;
  }
};

function teardown(): void {
  if (statsTimer !== null) {
    clearInterval(statsTimer);
    statsTimer = null;
  }
  driver?.stop();
  driver = null;
}
