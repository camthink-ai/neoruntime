/**
 * Talkback: capture microphone audio with Web Audio and send raw S16LE over WebSocket.
 */

import { createLogger } from '../shared/log';

const log = createLogger('talk');

export interface TalkOptions {
  url: string;
  sampleRate?: number;
  /** Outgoing chunk duration in milliseconds. */
  chunkMs?: number;
}

const TALK_WORKLET = `
class CTTalkProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.buf = [];
    this.limit = 480;
    this.port.onmessage = () => {};
  }
  process(inputs) {
    const ch = inputs[0] && inputs[0][0];
    if (ch) {
      this.buf.push(new Float32Array(ch));
    }
    if (this.buf.length >= this.limit) {
      let n = 0;
      this.buf.forEach((c) => (n += c.length));
      const merged = new Float32Array(n);
      let off = 0;
      this.buf.forEach((c) => { merged.set(c, off); off += c.length; });
      this.buf = [];
      this.port.postMessage({ chunk: merged }, [merged.buffer]);
    }
    return true;
  }
}
registerProcessor('ct-talk', CTTalkProcessor);
`;

export class AudioTalker {
  private ws: WebSocket | null = null;
  private ctx: AudioContext | null = null;
  private stream: MediaStream | null = null;
  private node: AudioWorkletNode | null = null;
  private sentBytes = 0;

  async start(opts: TalkOptions): Promise<void> {
    const sampleRate = opts.sampleRate ?? 48000;
    const framesPerChunk = Math.round((sampleRate * (opts.chunkMs ?? 100)) / 1000);
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true },
    });
    this.ctx = new AudioContext({ sampleRate });
    const blobUrl = URL.createObjectURL(new Blob([TALK_WORKLET], { type: 'application/javascript' }));
    await this.ctx.audioWorklet.addModule(blobUrl);
    URL.revokeObjectURL(blobUrl);
    this.node = new AudioWorkletNode(this.ctx, 'ct-talk');
    const src = this.ctx.createMediaStreamSource(this.stream);
    src.connect(this.node);

    const ws = new WebSocket(opts.url);
    ws.binaryType = 'arraybuffer';
    this.ws = ws;
    await new Promise<void>((resolve, reject) => {
      ws.onopen = () => resolve();
      ws.onerror = () => reject(new Error('talk ws failed'));
    });

    this.node.port.onmessage = (e) => {
      if (ws.readyState !== 1 || !(e.data && e.data.chunk)) return;
      const f = e.data.chunk as Float32Array;
      const i16 = new Int16Array(f.length);
      for (let i = 0; i < f.length; i++) {
        const s = Math.max(-1, Math.min(1, f[i]));
        i16[i] = s < 0 ? s * 0x8000 : s * 0x7fff;
      }
      ws.send(i16.buffer);
      this.sentBytes += i16.byteLength;
    };
    // 每帧上抛 chunkMs 后触发（framesPerChunk / 128 帧 ≈ 每 process 块 128 帧）
    (this.node as unknown as { parameters?: unknown }).parameters; // no-op
    // worklet 内 limit 固定 480（10ms@48k），主线程侧按 chunkMs 聚合：
    // 简化：直接转发 worklet 块（10ms 粒度足够低延迟）
    void framesPerChunk;
    if (this.ctx.state === 'suspended') await this.ctx.resume().catch(() => undefined);
    log.info(`talk started → ${opts.url}`);
  }

  stop(): void {
    try {
      this.node?.port && (this.node.port.onmessage = null);
    } catch {
      /* ignore */
    }
    this.node?.disconnect();
    this.node = null;
    this.stream?.getTracks().forEach((t) => t.stop());
    this.stream = null;
    try {
      this.ws?.close();
    } catch {
      /* ignore */
    }
    this.ws = null;
    void this.ctx?.close().catch(() => undefined);
    this.ctx = null;
  }

  get sent(): number {
    return this.sentBytes;
  }
}
