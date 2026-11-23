// @ts-nocheck
/**
 * PCM audio channel: receive raw S16LE over WebSocket and play it in an inline
 * AudioWorklet. Configure source.audio to enable it, or set enabled to false.
 */

import { PcmRing } from './pcm-ring';
import { ReconnectController, DEFAULT_RECONNECT_POLICY } from '../transport/reconnect';
import { createLogger } from '../shared/log';

const log = createLogger('audio');

export interface PcmAudioOptions {
  url: string;
  sampleRate?: number;
  channels?: number;
  parsePacket?: (message: Uint8Array) => { pcm: Uint8Array; pts90k: number } | null;
  /** Prebuffer duration in milliseconds for startup and underruns. */
  prebufferMs?: number;
  reconnect?: Partial<typeof DEFAULT_RECONNECT_POLICY>;
}

export const WORKLET_SRC = `
class CTPcmProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.queue = [];
    this.frames = 0;
    this.prebufferFrames = 48000 * 0.3;
    this.underruns = 0;
    this.buffering = true;
    this.reported = 0;
    this.paused = false;
    this.port.onmessage = (e) => {
      const m = e.data;
      if (m.cmd === 'config') this.prebufferFrames = m.prebufferFrames;
      else if (m.cmd === 'reset') { this.queue = []; this.frames = 0; this.buffering = true; this.port.postMessage({ev:'clock-reset'}); }
      else if (m.cmd === 'pause') { this.paused = true; this.queue = []; this.frames = 0; this.buffering = true; this.port.postMessage({ev:'clock-reset'}); }
      else if (m.cmd === 'resume') this.paused = false;
      else if (m.chunk) {
        this.queue.push({ samples: new Float32Array(m.chunk), pts90k: m.pts90k, offset: 0 });
        this.frames += m.chunk.length;
        while (this.frames > sampleRate * 1.2 && this.queue.length > 1) {
          const c = this.queue.shift();
          this.frames -= c.samples.length - c.offset;
        }
      }
    };
    this.port.postMessage({ ev: 'ready' });
  }
  process(_inputs, outputs) {
    const out = outputs[0][0];
    if (this.paused) { out.fill(0); return true; }
    if (this.buffering && this.frames < this.prebufferFrames) {
      out.fill(0);
      return true; // Prebuffering.
    }
    this.buffering = false;
    let off = 0;
    let endPts;
    while (off < out.length && this.queue.length > 0) {
      const c = this.queue[0];
      const take = Math.min(c.samples.length - c.offset, out.length - off);
      out.set(c.samples.subarray(c.offset, c.offset + take), off);
      c.offset += take;
      endPts = typeof c.pts90k === 'number' ? c.pts90k + c.offset * 90000 / sampleRate : undefined;
      if (c.offset === c.samples.length) this.queue.shift();
      this.frames -= take;
      off += take;
    }
    if (off < out.length) {
      out.fill(0, off);
      this.underruns += 1;
      this.buffering = true;
      this.port.postMessage({ev:'clock-reset'});
    } else if (typeof endPts === 'number') {
      this.reported += out.length;
      if (this.reported >= sampleRate / 20) {
        this.reported = 0;
        this.port.postMessage({ev:'clock', pts90k:endPts, contextTime:currentTime + out.length / sampleRate});
      }
    }
    return true;
  }
}
registerProcessor('ct-pcm', CTPcmProcessor);
`;

export class PcmAudioChannel {
  private ws: WebSocket | null = null;
  private ctx: AudioContext | null = null;
  private node: AudioWorkletNode | null = null;
  private ring = new PcmRing(48000);
  private reconnect = new ReconnectController(DEFAULT_RECONNECT_POLICY);
  private timer: ReturnType<typeof setInterval> | null = null;
  private closed = false;
  private paused = false;
  private readonly sampleRate: number;
  private readonly prebufferMs: number;
  private clockAnchor: { pts90k: number; contextTime: number; receivedAt: number } | null = null;

  constructor(private readonly opts: PcmAudioOptions) {
    this.sampleRate = opts.sampleRate ?? 48000;
    this.prebufferMs = opts.prebufferMs ?? 300;
    this.ring = new PcmRing(this.sampleRate);
  }

  async start(): Promise<void> {
    if (this.closed) return;
    const Ctx = window.AudioContext;
    this.ctx = new Ctx({ sampleRate: this.sampleRate });
    const blobUrl = URL.createObjectURL(new Blob([WORKLET_SRC], { type: 'application/javascript' }));
    await this.ctx.audioWorklet.addModule(blobUrl);
    URL.revokeObjectURL(blobUrl);
    this.node = new AudioWorkletNode(this.ctx, 'ct-pcm', { outputChannelCount: [1] });
    this.node.port.onmessage = (event: MessageEvent) => {
      const msg = event.data;
      if (msg?.ev === 'clock' && Number.isFinite(msg.pts90k) && Number.isFinite(msg.contextTime)) {
        this.clockAnchor = { pts90k: msg.pts90k, contextTime: msg.contextTime, receivedAt: performance.now() };
      } else if (msg?.ev === 'clock-reset') this.clockAnchor = null;
    };
    this.node.port.postMessage({
      cmd: 'config',
      prebufferFrames: Math.round((this.sampleRate * this.prebufferMs) / 1000),
    });
    this.node.connect(this.ctx.destination);
    if (this.ctx.state === 'suspended') await this.ctx.resume().catch(() => undefined);
    this.connect();
    // 主线程泵：环缓冲 → worklet（限幅防 worklet 内积压）
    this.timer = setInterval(() => this.pump(), 100);
  }

  private connect(): void {
    if (this.closed) return;
    let ws: WebSocket;
    try {
      ws = new WebSocket(this.opts.url);
    } catch (err) {
      log.warn('ws failed', err);
      return;
    }
    this.ws = ws;
    ws.binaryType = 'arraybuffer';
    ws.onopen = () => this.reconnect.onOpen();
    ws.onmessage = (ev) => {
      if (this.paused) return;
      if (!(ev.data instanceof ArrayBuffer)) return;
      const message = new Uint8Array(ev.data);
      let packet: { pcm: Uint8Array; pts90k: number } | null = null;
      if (this.opts.parsePacket) {
        try { packet = this.opts.parsePacket(message); } catch (err) { log.warn('audio packet parse failed', err); return; }
        if (!packet || !Number.isFinite(packet.pts90k) || packet.pcm.byteLength % 2 !== 0) return;
      }
      const pcm = packet?.pcm ?? message;
      if (pcm.byteLength % 2 !== 0) return;
      const copy = pcm.slice();
      const samples = PcmRing.toFloat(new Int16Array(copy.buffer));
      if (packet && this.node) this.node.port.postMessage({ chunk: samples, pts90k: packet.pts90k }, [samples.buffer]);
      else this.ring.push(samples);
    };
    ws.onclose = (ev) => {
      if (this.ws !== ws || this.closed) return;
      this.ws = null;
      this.clockAnchor = null;
      this.node?.port.postMessage({ cmd: 'reset' });
      const d = this.reconnect.onClose(ev.code, ev.reason);
      if (d.action === 'reconnect') {
        setTimeout(() => this.connect(), d.delayMs);
      }
    };
    ws.onerror = () => undefined;
  }

  private pump(): void {
    if (!this.node || this.closed) return;
    // worklet 端自带预缓冲与上限，主线程按到达顺序转发即可
    while (this.ring.bufferedFrames > 0) {
      const chunk = this.ring.pull(this.ring.bufferedFrames)!;
      this.node.port.postMessage({ chunk }, [chunk.buffer]);
    }
  }

  /** Current buffered duration in milliseconds for A/V synchronization. */
  get bufferedMs(): number {
    return this.ring.bufferedMs;
  }

  /** Whether the audio master clock is running and producing output. */
  get active(): boolean {
    return !!this.node && !this.closed;
  }

  /** Estimate the PTS currently reaching the audio output, in 90 kHz ticks. */
  get playbackPts90k(): number | null {
    const anchor = this.clockAnchor;
    const ctx = this.ctx;
    if (!anchor || !ctx || ctx.state !== 'running' || performance.now() - anchor.receivedAt > 500) return null;
    const timestamp = ctx.getOutputTimestamp?.();
    const outputTime = timestamp && typeof timestamp.contextTime === 'number' && timestamp.contextTime > 0 && typeof timestamp.performanceTime === 'number'
      ? timestamp.contextTime + (performance.now() - timestamp.performanceTime) / 1000
      : ctx.currentTime - (ctx.outputLatency || 0);
    return anchor.pts90k + (outputTime - anchor.contextTime) * 90000;
  }

  suspend(): void {
    this.paused = true;
    this.clockAnchor = null;
    this.ring.clear();
    this.node?.port.postMessage({ cmd: 'pause' });
  }

  resume(): void {
    this.paused = false;
    this.node?.port.postMessage({ cmd: 'resume' });
  }

  close(): void {
    this.closed = true;
    this.clockAnchor = null;
    if (this.timer !== null) clearInterval(this.timer);
    this.timer = null;
    try {
      this.ws?.close();
    } catch {
      /* ignore */
    }
    this.ws = null;
    this.node?.disconnect();
    this.node = null;
    void this.ctx?.close().catch(() => undefined);
    this.ctx = null;
  }
}
