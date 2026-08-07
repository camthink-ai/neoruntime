/**
 * AudioPlayer — Live PCM audio via WebSocket + Web Audio API
 *
 * Prefers AudioWorklet (dedicated audio thread, no main-thread blocking)
 * and falls back to ScriptProcessorNode for embedded browsers that don't
 * support AudioWorklet (e.g., older WebKit on Hailo-15 devices).
 *
 * Both paths receive raw PCM chunks (S16LE, 48 kHz, mono) from the
 * platform-api audio WebSocket and include A/V sync pre-buffering.
 */

const SAMPLE_RATE = 48000;
const DEFAULT_SYNC_DELAY_MS = 300;
const FALLBACK_BUFFER_SIZE = 2048; // ~43 ms at 48 kHz

interface AudioPlayerOptions {
  /** Delay audio output by this many milliseconds to match video MSE latency. Default: 300 */
  syncDelayMs?: number;
  /** Callback when the sync pre-buffer is filled and audio output starts */
  onPlaying?: () => void;
}

export class AudioPlayer {
  private ws: WebSocket | null = null;

  private audioCtx: AudioContext | null = null;

  private gainNode: GainNode | null = null;

  private reconnectTimer: ReturnType<typeof setTimeout> | null = null;

  private url = '';

  private activeFlag = false;

  private volumeLevel = 1.0;

  private syncDelayMs: number;

  private syncDelaySamples: number;

  private onPlayingCb: (() => void) | null = null;

  // --- AudioWorklet path ---
  private workletNode: AudioWorkletNode | null = null;

  // --- ScriptProcessorNode fallback path ---
  private processor: ScriptProcessorNode | null = null;

  private ringBuffer: Float32Array = new Float32Array(SAMPLE_RATE * 2); // 2s ring

  private readPos = 0;

  private writePos = 0;

  private outputStarted = false;

  get active(): boolean {
    return this.activeFlag;
  }

  setVolume(v: number): void {
    this.volumeLevel = Math.max(0, Math.min(1, v));
    if (this.gainNode) {
      this.gainNode.gain.value = this.volumeLevel;
    }
  }

  get volume(): number {
    return this.volumeLevel;
  }

  constructor(opts?: AudioPlayerOptions) {
    this.syncDelayMs = opts?.syncDelayMs ?? DEFAULT_SYNC_DELAY_MS;
    this.syncDelaySamples = Math.round((this.syncDelayMs / 1000) * SAMPLE_RATE);
    this.onPlayingCb = opts?.onPlaying ?? null;
  }

  start(url: string): void {
    this.url = url;
    this.activeFlag = true;

    // Create/resume the context synchronously when start() is called from a
    // user gesture. Waiting for WebSocket.onopen can lose browser activation.
    if (!this.audioCtx || this.audioCtx.state === 'closed') {
      this.audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE });
    }
    if (this.audioCtx.state === 'suspended') {
      this.audioCtx.resume().catch(() => {});
    }

    this.connect();
  }

  /**
   * Resume the AudioContext if it was created suspended. This happens on a
   * cold page-load with no prior user activation in the document (the normal
   * flow arrives here via in-app navigation, which already grants sticky
   * activation, so the context starts running and this is a no-op). The player
   * exposes this so the gesture-fallback listener can resume playback on the
   * first interaction without recreating the whole WS+worklet graph.
   */
  resume(): void {
    if (this.audioCtx && this.audioCtx.state === 'suspended') {
      this.audioCtx.resume().catch(() => {});
    }
  }

  setSyncDelay(ms: number): void {
    this.syncDelayMs = ms;
    this.syncDelaySamples = Math.round((ms / 1000) * SAMPLE_RATE);
    // Forward to worklet if active
    if (this.workletNode) {
      this.workletNode.port.postMessage({
        type: 'config',
        syncDelayMs: ms,
      });
    }
  }

  stop(): void {
    this.activeFlag = false;
    this.cleanup();
  }

  // ---- AudioWorklet path (preferred) ----

  private async initAudioWorklet(): Promise<boolean> {
    if (!this.audioCtx || this.audioCtx.state === 'closed') {
      this.audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE });
    }

    if (!this.audioCtx.audioWorklet) {
      return false; // not supported
    }

    await this.audioCtx.audioWorklet.addModule('/pcm-processor.js');

    this.workletNode = new AudioWorkletNode(this.audioCtx, 'pcm-processor');

    this.workletNode.port.onmessage = (e: MessageEvent) => {
      if (e.data?.type === 'playing' && this.onPlayingCb) {
        this.onPlayingCb();
      }
    };

    this.workletNode.port.postMessage({
      type: 'config',
      syncDelayMs: this.syncDelayMs,
    });

    this.gainNode = this.audioCtx.createGain();
    this.gainNode.gain.value = this.volumeLevel;

    this.workletNode.connect(this.gainNode);
    this.gainNode.connect(this.audioCtx.destination);

    return true;
  }

  // ---- ScriptProcessorNode fallback ----

  private initAudioFallback(): void {
    if (!this.audioCtx) {
      this.audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE });
    }

    this.readPos = 0;
    this.writePos = 0;
    this.outputStarted = false;

    this.processor = this.audioCtx.createScriptProcessor(
      FALLBACK_BUFFER_SIZE,
      0,
      1
    );
    this.processor.onaudioprocess = e => this.onAudioProcess(e);

    this.gainNode = this.audioCtx.createGain();
    this.gainNode.gain.value = this.volumeLevel;

    this.processor.connect(this.gainNode);
    this.gainNode.connect(this.audioCtx.destination);
  }

  private enqueuePcm(pcm: Int16Array): void {
    const ctx = this.audioCtx;
    if (!ctx || ctx.state === 'closed') return;

    if (ctx.state === 'suspended') {
      ctx.resume();
    }

    // Convert S16LE → Float32 and write to ring buffer
    for (let i = 0; i < pcm.length; i++) {
      this.ringBuffer[this.writePos] = pcm[i] / 32768;
      this.writePos = (this.writePos + 1) % this.ringBuffer.length;
    }

    // Start audio output once enough samples are buffered
    if (!this.outputStarted && this.ringAvailable() >= this.syncDelaySamples) {
      this.outputStarted = true;
      if (this.onPlayingCb) this.onPlayingCb();
    }
  }

  private onAudioProcess(e: AudioProcessingEvent): void {
    const output = e.outputBuffer.getChannelData(0);

    if (!this.outputStarted) {
      output.fill(0);
      return;
    }

    const available = this.ringAvailable();

    if (available < output.length) {
      // Underrun — fill with silence
      output.fill(0);
      for (let i = 0; i < available; i++) {
        output[i] = this.ringBuffer[this.readPos];
        this.readPos = (this.readPos + 1) % this.ringBuffer.length;
      }
      return;
    }

    for (let i = 0; i < output.length; i++) {
      output[i] = this.ringBuffer[this.readPos];
      this.readPos = (this.readPos + 1) % this.ringBuffer.length;
    }
  }

  private ringAvailable(): number {
    const w = this.writePos;
    const r = this.readPos;
    return w >= r ? w - r : this.ringBuffer.length - r + w;
  }

  // ---- Connection ----

  private connect(): void {
    if (!this.activeFlag) return;

    this.ws = new WebSocket(this.url);
    this.ws.binaryType = 'arraybuffer';

    // Buffer packets until either audio path is fully initialized.
    const pending: ArrayBuffer[] = [];
    let useWorklet = false;
    let audioInitialized = false;

    this.ws.onopen = async () => {
      try {
        useWorklet = await this.initAudioWorklet();
        if (!useWorklet) {
          console.warn(
            '[AudioPlayer] AudioWorklet unavailable, falling back to ScriptProcessorNode'
          );
          this.initAudioFallback();
        }
      } catch (err) {
        console.error(
          '[AudioPlayer] AudioWorklet init failed, falling back to ScriptProcessorNode:',
          err
        );
        useWorklet = false;
        this.initAudioFallback();
      }

      audioInitialized = true;

      // Flush packets that arrived while the audio path was initializing.
      if (useWorklet && this.workletNode) {
        for (const buf of pending) {
          this.workletNode.port.postMessage({ type: 'pcm', buffer: buf }, [
            buf,
          ]);
        }
      } else {
        for (const buf of pending) {
          this.enqueuePcm(new Int16Array(buf));
        }
      }
      pending.length = 0;
    };

    this.ws.onmessage = (event: MessageEvent) => {
      if (!(event.data instanceof ArrayBuffer)) return;

      if (!audioInitialized) {
        pending.push(event.data);
        return;
      }

      // Worklet path: forward to processor.
      if (useWorklet) {
        if (this.audioCtx?.state === 'suspended') {
          this.audioCtx.resume();
        }

        this.workletNode!.port.postMessage(
          { type: 'pcm', buffer: event.data },
          [event.data]
        );
        return;
      }

      // Fallback path: enqueue into ring buffer
      this.enqueuePcm(new Int16Array(event.data));
    };

    this.ws.onclose = () => {
      this.scheduleReconnect();
    };

    this.ws.onerror = () => {
      // onclose will fire after this
    };
  }

  private scheduleReconnect(): void {
    if (!this.activeFlag) return;
    this.cleanupAudio();
    this.reconnectTimer = setTimeout(() => this.connect(), 2000);
  }

  private cleanupAudio(): void {
    if (this.workletNode) {
      this.workletNode.disconnect();
      this.workletNode = null;
    }
    if (this.processor) {
      this.processor.disconnect();
      this.processor = null;
    }
    if (this.gainNode) {
      this.gainNode.disconnect();
      this.gainNode = null;
    }
    if (this.audioCtx) {
      this.audioCtx.close().catch(() => {});
      this.audioCtx = null;
    }
  }

  private cleanup(): void {
    if (this.reconnectTimer) {
      clearTimeout(this.reconnectTimer);
      this.reconnectTimer = null;
    }
    if (this.ws) {
      this.ws.onclose = null;
      this.ws.onerror = null;
      this.ws.close();
      this.ws = null;
    }
    this.cleanupAudio();
  }
}
