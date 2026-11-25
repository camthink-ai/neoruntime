/**
 * CTPlayer orchestrates transport, pipelines, fallback, and health monitoring.
 * Public methods expose playback control, snapshots, stats, and events.
 */

import {
  EventBus,
  type CTEventMap,
  type FallbackEvent,
  type Unsubscribe,
} from '../shared/events';
import type {
  CTPlayerOptions,
  CTSource,
  CTStats,
  EncodedFrame,
  FallbackPolicy,
  LatencyPresetName,
  PlayMode,
  PlayerState,
  StreamChangeRequest,
  StreamChangeResponse,
  TechKind,
  TransportStatus,
  VideoCodec,
} from '../shared/types';
import { ctError, type CTErrorCode } from '../shared/errors';
import { setLogLevel, createLogger } from '../shared/log';
import { detectCapabilities, type CapabilityProfile } from '../shared/capability';
import { WsTransport } from '../transport/ws';
import { RenderTargetManager } from '../render/render-target';
import type { Pipeline, PipelineContext } from '../pipeline/types';
import type { WasmKernelOptions } from '../wasm/kernel-client';
import { MsePipeline } from '../pipeline/mse-pipeline';
import { WebCodecsPipeline } from '../pipeline/webcodecs-pipeline';
import { WasmPipeline } from '../pipeline/wasm-pipeline';
import { FallbackMachine, type FsmAction, type FsmInput } from './fallback';
import { HealthMonitor } from './health';
import { resolveLatencyParams, type LatencyParams } from './presets';
import { PcmAudioChannel } from '../audio/pcm-channel';
import { AudioTalker } from '../audio/talker';

const log = createLogger('player');

export class CTPlayer {
  readonly bus = new EventBus();

  private render: RenderTargetManager | null = null;
  private transport: WsTransport | null = null;
  private pipeline: Pipeline | null = null;
  private machine = new FallbackMachine();
  private health: HealthMonitor | null = null;
  private capability: CapabilityProfile | null = null;
  private latencyName: LatencyPresetName;
  private latency: LatencyParams | null = null;
  private readonly policy: FallbackPolicy;
  private sourceRef: CTSource | null = null;
  private mode: PlayMode = 'live';
  private readonly wasmOptions: WasmKernelOptions | undefined;
  private objectFit: 'contain' | 'cover';
  private audio: PcmAudioChannel | null = null;
  private audioSyncMaxDriftMs: number | null = null;
  private talker: AudioTalker | null = null;
  private codecDetected: VideoCodec | null = null;
  private parameterSets = new Map<number, Uint8Array>();
  private wasmNeedsParameters = false;
  private state: PlayerState = 'idle';
  private destroyed = false;
  private playing = false;
  private sessionGeneration = 0;

  // Statistics
  private statsTimer: ReturnType<typeof setInterval> | null = null;
  private framesThisSecond = 0;
  private fps = 0;
  private renderedFps: number | null = null;
  private lastRenderPipeline: Pipeline | null = null;
  private lastFramesOut = 0;
  private pps = 0;
  private bitrateKBps = 0;
  private lastTransportStats = { bytes: 0, messages: 0 };
  private fallbackCount = 0;
  private reconnectCount = 0;
  private droppedTotal = 0;

  constructor(opts: CTPlayerOptions = {}) {
    this.objectFit = opts.objectFit ?? 'contain';
    this.latencyName = opts.latency ?? 'balanced';
    this.policy = {
      allowStreamFallback: true,
      allowQualityFallback: true,
      maxInternalRestarts: 3,
      ...opts.fallback,
    };
    this.wasmOptions = opts.wasm
      ? { baseUrl: opts.wasm.kernelBaseUrl, threads: opts.wasm.threads }
      : undefined;
    setLogLevel(opts.logLevel ?? 'warn');
  }

  attach(target: HTMLElement | string): this {
    if (!this.render) {
      this.render = new RenderTargetManager({ objectFit: this.objectFit });
    }
    this.render.attach(target);
    return this;
  }

  async play(source: CTSource): Promise<void> {
    if (this.destroyed) throw new Error('player destroyed');
    if (!this.render) throw new Error('call attach() before play()');
    this.stopSession();
    const generation = this.sessionGeneration;
    this.playing = true;
    this.sourceRef = source;
    this.mode = source.mode ?? 'live';
    this.capability = await detectCapabilities();
    if (this.destroyed || generation !== this.sessionGeneration) return;
    this.latency = resolveLatencyParams(this.latencyName, this.capability.platform);
    this.codecDetected =
      source.codec && source.codec !== 'auto' ? source.codec : null;
    this.machine = new FallbackMachine();
    this.fallbackCount = 0;
    this.droppedTotal = 0;
    this.renderedFps = null;
    this.lastRenderPipeline = null;
    this.lastFramesOut = 0;

    this.health = new HealthMonitor({
      onStall: () => this.dispatchMachine({ type: 'stall' }),
      onErrorBurst: () => this.dispatchMachine({ type: 'fatal', code: 'DECODE_ERROR' }),
    });
    this.health.start();

    this.setState('connecting');
    this.transport = new WsTransport();
    this.transport.onStatus((s) => this.onTransportStatus(s));
    this.transport.onFrame((f) => this.onFrame(f));
    this.transport.open(source.transport.url, source.adapter, {
      reconnect: source.transport.reconnect,
    });
    this.startStatsTimer();

    // Audio is enabled by configuration and can be temporarily disabled with enabled: false.
    if (source.audio && source.audio.enabled !== false) {
      this.audio = new PcmAudioChannel({
        url: source.audio.url,
        sampleRate: source.audio.sampleRate,
        channels: source.audio.channels,
        parsePacket: source.audio.parsePacket,
      });
      this.audioSyncMaxDriftMs =
        source.audio.sync === false ? null : (source.audio.sync?.maxDriftMs ?? 150);
      void this.audio.start().catch((err) => {
        this.bus.emit(
          'error',
          ctError('NETWORK_ERROR', { fatal: false, detail: `audio: ${String(err)}` })
        );
        this.audio?.close();
        this.audio = null;
      });
    }
  }

  /** Playback mode uses MSE for buffering and seeking. */
  private effectivePolicy(): FallbackPolicy {
    if (this.mode !== 'playback') return this.policy;
    return { ...this.policy, techChain: ['mse'] };
  }

  /** Playback: seek to a buffered position in seconds. */
  seek(sec: number): void {
    if (this.mode !== 'playback') return;
    this.pipeline?.seek?.(sec);
  }

  /** Playback: set speed from 0.25 to 4x; unavailable in live mode. */
  setSpeed(rate: number): void {
    if (this.mode !== 'playback') return;
    this.pipeline?.setSpeed?.(rate);
  }

  /** Playback: return timeline information. */
  getTimeline(): { durationMs: number | null; positionMs: number; ranges: Array<[number, number]> } {
    return this.pipeline?.timeline?.() ?? { durationMs: null, positionMs: 0, ranges: [] };
  }

  pause(): void {
    this.pipeline?.suspend();
    this.audio?.suspend();
    if (this.state !== 'error') this.setState('paused');
  }

  resume(): void {
    this.audio?.resume();
    this.pipeline?.resume();
    if (this.pipeline && this.state === 'paused') this.setState('playing');
  }

  async snapshot(): Promise<Blob> {
    return this.render!.snapshot();
  }

  setLatency(preset: LatencyPresetName): void {
    this.latencyName = preset;
    // Takes effect on the next play call, when the pipeline is created.
  }

  /** Update the render fit at runtime; pipelines apply it from the next frame. */
  setObjectFit(fit: 'contain' | 'cover'): void {
    this.objectFit = fit;
    this.render?.setObjectFit(fit);
    this.pipeline?.setObjectFit?.(fit);
  }

  getStats(): CTStats {
    const info = this.pipeline?.latencyInfo() ?? { latencyMs: null, bufferedMs: null };
    const pipelineStats = this.pipeline?.stats();
    return {
      state: this.state,
      tech: this.machine.currentTech,
      codec: this.codecDetected,
      fps: this.fps,
      renderedFps: this.renderedFps,
      pps: this.pps,
      bitrateKBps: this.bitrateKBps,
      bytes: this.lastTransportStats.bytes,
      latencyMs: info.latencyMs,
      bufferedMs: info.bufferedMs,
      droppedFrames: pipelineStats?.dropped ?? this.droppedTotal,
      ...(pipelineStats?.videoSize
        ? { videoWidth: pipelineStats.videoSize.width, videoHeight: pipelineStats.videoSize.height }
        : {}),
      ...(pipelineStats ? { pipelineFramesIn: pipelineStats.framesIn } : {}),
      ...(pipelineStats?.drawTiming ? { drawTiming: pipelineStats.drawTiming } : {}),
      ...(pipelineStats?.decodeErrors !== undefined ? { decodeErrors: pipelineStats.decodeErrors } : {}),
      ...(pipelineStats?.wasmDrops ? { wasmDrops: pipelineStats.wasmDrops } : {}),
      ...(pipelineStats?.wasmPresentation ? { wasmPresentation: pipelineStats.wasmPresentation } : {}),
      ...(pipelineStats?.avSync ? { avSync: pipelineStats.avSync } : {}),
      ...(pipelineStats?.wasmPerf ? { wasmPerf: pipelineStats.wasmPerf } : {}),
      fallbacks: this.fallbackCount,
      reconnects: this.reconnectCount,
    };
  }

  on<K extends keyof CTEventMap>(key: K, handler: (payload: CTEventMap[K]) => void): Unsubscribe {
    return this.bus.on(key, handler);
  }

  /** Talkback: stream microphone audio over WebSocket as S16LE. */
  async startTalk(url: string, opts?: { sampleRate?: number; chunkMs?: number }): Promise<void> {
    this.stopTalk();
    this.talker = new AudioTalker();
    await this.talker.start({ url, ...opts });
  }

  stopTalk(): void {
    this.talker?.stop();
    this.talker = null;
  }

  destroy(): void {
    this.destroyed = true;
    this.playing = false;
    this.stopTalk();
    this.stopSession();
    this.render?.destroy();
    this.render = null;
    this.bus.clear();
  }

  // -------------------------------------------------------------------------

  private stopSession(): void {
    this.sessionGeneration += 1;
    if (this.statsTimer !== null) {
      clearInterval(this.statsTimer);
      this.statsTimer = null;
    }
    this.audio?.close();
    this.audio = null;
    this.audioSyncMaxDriftMs = null;
    this.health?.stop();
    this.health = null;
    this.transport?.destroy();
    this.transport = null;
    this.pipeline?.destroy();
    this.pipeline = null;
    this.codecDetected = null;
    this.parameterSets.clear();
    this.wasmNeedsParameters = false;
    this.playing = false;
    this.lastTransportStats = { bytes: 0, messages: 0 };
  }

  private setState(state: PlayerState, detail?: string): void {
    this.state = state;
    this.bus.emit('state', { state, tech: this.machine.currentTech, detail });
  }

  private onTransportStatus(s: TransportStatus): void {
    switch (s.status) {
      case 'connecting':
        if (!this.pipeline) this.setState('connecting');
        break;
      case 'open':
        if (this.health) this.health.isOpen = true;
        if (!this.pipeline) this.setState('buffering');
        break;
      case 'reconnecting':
        this.reconnectCount += 1;
        if (this.pipeline) this.setState('degraded', `reconnect #${s.attempt}`);
        break;
      case 'closed':
        if (this.health) this.health.isOpen = false;
        this.bus.emit(
          'error',
          ctError('NETWORK_ERROR', { fatal: false, detail: { code: s.code, reason: s.reason } })
        );
        if (!this.pipeline) this.setState('idle', s.reason);
        break;
      case 'superseded':
        if (this.health) this.health.isOpen = false;
        this.bus.emit(
          'error',
          ctError('SUPERSEDED', { fatal: true, detail: { code: s.code, reason: s.reason } })
        );
        this.setState('error', 'superseded');
        break;
      case 'retry-exhausted':
        if (this.health) this.health.isOpen = false;
        this.bus.emit('error', ctError('RETRY_EXHAUSTED', { fatal: true }));
        this.setState('error', 'retry exhausted');
        break;
      case 'parse-error':
        if (this.health) this.health.isOpen = false;
        this.bus.emit('error', ctError('PROTOCOL_PARSE_ERROR', { fatal: true, detail: s.message }));
        this.setState('error', s.message);
        break;
      case 'error':
        this.bus.emit('error', ctError('NETWORK_ERROR', { fatal: false, detail: s.message }));
        break;
    }
  }

  private onFrame(f: EncodedFrame): void {
    this.health?.markFrame();
    this.framesThisSecond += 1;
    if (!this.playing) return;

    if (this.codecDetected && f.codec !== this.codecDetected) this.parameterSets.clear();
    const parameterTypes = f.codec === 'h265' ? [32, 33, 34] : [7, 8];
    for (const nalu of f.nalus) {
      if (parameterTypes.includes(nalu.type)) {
        this.parameterSets.set(nalu.type, f.data.slice(nalu.offset, nalu.offset + nalu.size));
      }
    }

    if (this.machine.state === 'idle') {
      this.codecDetected = f.codec;
      this.dispatchMachine({
        type: 'start',
        codec: f.codec,
        capability: this.capability!,
        policy: this.effectivePolicy(),
      });
      // The first access unit can carry the only parameter sets and initial IDR.
      // Pipeline selection is synchronous; forward that unit to the selected pipeline.
      this.submitFrame(f);
      return;
    }
    if (f.codec !== this.codecDetected) {
      // Stream fallback completed and the new codec has arrived.
      this.codecDetected = f.codec;
      this.dispatchMachine({ type: 'stream-arrived', codec: f.codec });
      this.submitFrame(f);
      return;
    }
    this.submitFrame(f);
  }

  private submitFrame(frame: EncodedFrame): void {
    if (this.pipeline?.tech === 'wasm' && this.wasmNeedsParameters && frame.keyframe) {
      const types = frame.codec === 'h265' ? [32, 33, 34] : [7, 8];
      const missing = types.filter((type) => !frame.nalus.some((nalu) => nalu.type === type));
      const sets = missing.map((type) => ({ type, data: this.parameterSets.get(type) }));
      if (sets.every((set) => set.data)) {
        const prefixSize = sets.reduce((size, set) => size + set.data!.length, 0);
        const data = new Uint8Array(prefixSize + frame.data.length);
        const nalus = [] as EncodedFrame['nalus'];
        let offset = 0;
        for (const set of sets) {
          data.set(set.data!, offset);
          nalus.push({ offset, size: set.data!.length, type: set.type });
          offset += set.data!.length;
        }
        data.set(frame.data, offset);
        nalus.push(...frame.nalus.map((nalu) => ({ ...nalu, offset: nalu.offset + offset })));
        frame = { ...frame, data, nalus };
        this.wasmNeedsParameters = false;
      }
    }
    this.pipeline?.submit(frame);
  }

  private dispatchMachine(input: FsmInput): void {
    const out = this.machine.dispatch(input);
    if (out.transition) {
      this.fallbackCount += 1;
      const t = out.transition;
      const ev: FallbackEvent = { from: t.from, to: t.to, level: t.level, reason: t.reason };
      log.info(`fallback: ${t.from} -> ${String(t.to)} (${t.level}, ${t.reason})`);
      this.bus.emit('fallback', ev);
    }
    for (const action of out.actions) {
      this.executeAction(action);
    }
  }

  private executeAction(action: FsmAction): void {
    switch (action.type) {
      case 'use-tech':
        this.createPipeline(action.tech);
        break;
      case 'restart-tech':
        this.pipeline?.destroy();
        this.pipeline = null;
        this.createPipeline(action.tech);
        break;
      case 'request-stream':
        void this.requestStreamSwitch(action.codec);
        break;
      case 'terminal':
        this.bus.emit('error', action.error);
        this.setState('error', action.error.code);
        break;
    }
  }

  private createPipeline(tech: TechKind): void {
    if (!this.render || !this.latency) return;
    this.pipeline?.destroy();
    this.pipeline = null;

    const ctx: PipelineContext = {
      render: this.render,
      latency: this.latency,
      objectFit: this.objectFit,
      mode: this.mode,
      wasm: this.wasmOptions,
      ...(this.sourceRef?.audio?.parsePacket && this.audioSyncMaxDriftMs !== null
        ? { audioClock: { getPts90k: () => this.audio?.playbackPts90k ?? null, maxDriftMs: this.audioSyncMaxDriftMs } }
        : {}),
      onFatal: (code: CTErrorCode, detail?: unknown) => {
        this.bus.emit(
          'error',
          ctError(code, { fatal: false, tech, detail, recovery: 'handled by fallback chain' })
        );
        if (code === 'CODEC_MISMATCH') {
          this.dispatchMachine({ type: 'codec-mismatch' });
        } else {
          this.dispatchMachine({ type: 'fatal', code });
        }
      },
      onReady: () => {
        this.dispatchMachine({ type: 'ready' });
        if (this.state !== 'error') this.setState('playing');
      },
      onDecodeError: () => this.health?.recordError(),
    };

    let pipeline: Pipeline | null = null;
    if (tech === 'webcodecs') pipeline = new WebCodecsPipeline(ctx);
    else if (tech === 'mse') pipeline = new MsePipeline(ctx);
    else if (tech === 'wasm') pipeline = new WasmPipeline(ctx);

    if (!pipeline) {
      this.dispatchMachine({ type: 'fatal', code: 'CONFIGURE_FAILED' });
      return;
    }
    this.pipeline = pipeline;
    this.wasmNeedsParameters = tech === 'wasm';
    pipeline.init();
    // Re-arm the watchdog for the fresh pipeline: a latched stall must not
    // block escalation when the new pipeline also sees no frames —
    // restart-tech otherwise depends on a frame arriving to clear the latch.
    this.health?.reset();
    log.info(`pipeline created: ${tech}`);
  }

  /**
   * Request a stream switch; the host responds with acceptance and an optional URL.
   * If no listener responds, the request times out, or the host declines, continue fallback or terminate.
   */
  private async requestStreamSwitch(codec: VideoCodec): Promise<void> {
    const request: StreamChangeRequest = {
      codec,
      reason: 'codec-mismatch',
      current: { codec: this.codecDetected ?? codec, tech: this.machine.currentTech },
    };
    if (!this.bus.hasHandlers('request-stream')) {
      log.warn('no request-stream handler; treat as declined');
      this.dispatchMachine({ type: 'stream-declined' });
      return;
    }
    this.pendingResponses = [];
    this.bus.emit('request-stream', request);
    const response = await this.waitStreamResponse(5000, this.pendingResponses);
    this.pendingResponses = null;
    if (!response || !response.accepted) {
      this.dispatchMachine({ type: 'stream-declined' });
      return;
    }
    if (response.url && response.url !== this.sourceRef?.transport.url) {
      // Reconnect to the replacement URL and wait for the new codec.
      log.info(`reconnecting to fallback stream url`);
      this.codecDetected = null; // Detect the codec again after the host changes the stream.
      this.transport?.close();
      this.transport = new WsTransport();
      this.transport.onStatus((s) => this.onTransportStatus(s));
      this.transport.onFrame((f) => this.onFrame(f));
      this.transport.open(response.url, this.sourceRef!.adapter, {
        reconnect: this.sourceRef!.transport.reconnect,
      });
    }
    // For a codec switch on the same URL, keep the connection and wait for the new stream.
  }

  private waitStreamResponse(
    timeoutMs: number,
    responses: StreamChangeResponse[]
  ): Promise<StreamChangeResponse | null> {
    // The host can call respondStreamChange from the synchronous event callback.
    return new Promise((resolve) => {
      const started = Date.now();
      const poll = setInterval(() => {
        if (responses.length > 0) {
          clearInterval(poll);
          resolve(responses[0]);
        } else if (Date.now() - started > timeoutMs) {
          clearInterval(poll);
          resolve(null);
        }
      }, 50);
    });
  }

  /** Report the result of a request-stream event. */
  respondStreamChange(response: StreamChangeResponse): void {
    this.pendingResponses?.push(response);
  }

  private pendingResponses: StreamChangeResponse[] | null = null;

  private startStatsTimer(): void {
    if (this.statsTimer !== null) clearInterval(this.statsTimer);
    this.statsTimer = setInterval(() => {
      this.fps = this.framesThisSecond;
      this.framesThisSecond = 0;
      const pipe = this.pipeline;
      if (pipe && (pipe.tech === 'wasm' || pipe.tech === 'webcodecs')) {
        const rendered = pipe.stats().framesOut;
        this.renderedFps = Math.max(0, rendered - (pipe === this.lastRenderPipeline ? this.lastFramesOut : 0));
        this.lastRenderPipeline = pipe;
        this.lastFramesOut = rendered;
      } else {
        this.renderedFps = null;
        this.lastRenderPipeline = pipe;
        this.lastFramesOut = 0;
      }
      const t = this.transport?.getStats() ?? { bytes: 0, messages: 0 };
      const dBytes = t.bytes - this.lastTransportStats.bytes;
      const dMsgs = t.messages - this.lastTransportStats.messages;
      this.bitrateKBps = Math.max(0, Math.round(dBytes / 1024));
      this.pps = dMsgs;
      if (t.bytes >= this.lastTransportStats.bytes) {
        this.lastTransportStats = { bytes: t.bytes, messages: t.messages };
      }
      // A/V sync uses audio as the master clock and seeks to the live edge when video falls too far behind.
      if (this.audio?.active && !this.sourceRef?.audio?.parsePacket && this.audioSyncMaxDriftMs !== null && this.pipeline) {
        const v = this.pipeline.latencyInfo().latencyMs;
        const a = this.audio.bufferedMs;
        if (v !== null && v - a > this.audioSyncMaxDriftMs + 200) {
          this.pipeline.seek?.(Number.NaN); // In live mode, seek(NaN) jumps to the live edge.
          log.info(`av-sync: video lag ${Math.round(v - a)}ms > threshold, seeking live edge`);
        }
      }
      this.bus.emit('stats', this.getStats());
    }, 1000);
  }
}

export function createPlayer(opts: CTPlayerOptions = {}): CTPlayer {
  return new CTPlayer(opts);
}
