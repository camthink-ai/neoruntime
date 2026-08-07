/**
 * AudioTalker — browser microphone → device speaker (push-to-talk)
 *
 * Capture-direction counterpart of AudioPlayer. Captures the microphone with
 * getUserMedia, resamples to S16LE 48 kHz mono via an AudioWorklet (with a
 * ScriptProcessorNode fallback for embedded WebKit), and streams PCM chunks
 * as binary WebSocket frames to GET /api/v1/audio/talk, which forwards them
 * into camera-daemon's StreamAudioPcm gRPC → ALSA playback → speaker.
 *
 * Lifecycle is driven by the UI: start(url) on pointer-down, stop() on
 * pointer-up/leave/blur. stop() is idempotent and safe on any error path.
 */

const SAMPLE_RATE = 48000;
const CHANNELS = 1;
const BYTES_PER_SAMPLE = 2;
const FALLBACK_BUFFER_SIZE = 4096; // ScriptProcessorNode buffer size
const CHUNK_SIZE = 1024; // ~21 ms at 48 kHz, mirrors mic-processor.js

export interface AudioTalkerRecording {
  blob: Blob;
  durationMs: number;
  pcmBytes: number;
  sampleRate: number;
  channels: number;
}

export interface AudioTalkerOptions {
  /** Called with a normalized mic level (0..1) for a level meter. */
  onLevel?: (level: number) => void;
  /** Called when the server rejects the session because another is active (HTTP 429). */
  onBusy?: () => void;
  /** Called when the session ends unexpectedly (WS close/error after open). */
  onError?: (message: string) => void;
  /** Called with the exact PCM sent to the device, wrapped as a WAV blob. */
  onRecording?: (recording: AudioTalkerRecording) => void;
}

const writeAscii = (view: DataView, offset: number, value: string): void => {
  for (let i = 0; i < value.length; i++) {
    view.setUint8(offset + i, value.charCodeAt(i));
  }
};

const createWavBlob = (
  chunks: ArrayBuffer[],
  pcmBytes: number,
  sampleRate: number,
  channels: number
): Blob => {
  const header = new ArrayBuffer(44);
  const view = new DataView(header);
  const blockAlign = channels * BYTES_PER_SAMPLE;
  const byteRate = sampleRate * blockAlign;

  writeAscii(view, 0, 'RIFF');
  view.setUint32(4, 36 + pcmBytes, true);
  writeAscii(view, 8, 'WAVE');
  writeAscii(view, 12, 'fmt ');
  view.setUint32(16, 16, true);
  view.setUint16(20, 1, true);
  view.setUint16(22, channels, true);
  view.setUint32(24, sampleRate, true);
  view.setUint32(28, byteRate, true);
  view.setUint16(32, blockAlign, true);
  view.setUint16(34, BYTES_PER_SAMPLE * 8, true);
  writeAscii(view, 36, 'data');
  view.setUint32(40, pcmBytes, true);

  return new Blob([header, ...chunks], { type: 'audio/wav' });
};

export class AudioTalker {
  private ws: WebSocket | null = null;

  private audioCtx: AudioContext | null = null;

  private stream: MediaStream | null = null;

  private source: MediaStreamAudioSourceNode | null = null;

  // AudioWorklet path
  private workletNode: AudioWorkletNode | null = null;

  // ScriptProcessorNode fallback path
  private processor: ScriptProcessorNode | null = null;

  private accum = new Float32Array(CHUNK_SIZE);

  private accumPos = 0;

  private running = false;

  private stopping = false;

  private stopPromise: Promise<void> | null = null;

  private onLevelCb: ((level: number) => void) | null = null;

  private onBusyCb: (() => void) | null = null;

  private onErrorCb: ((message: string) => void) | null = null;

  private onRecordingCb: ((recording: AudioTalkerRecording) => void) | null = null;

  private recordingChunks: ArrayBuffer[] = [];

  private recordingBytes = 0;

  constructor(opts?: AudioTalkerOptions) {
    this.onLevelCb = opts?.onLevel ?? null;
    this.onBusyCb = opts?.onBusy ?? null;
    this.onErrorCb = opts?.onError ?? null;
    this.onRecordingCb = opts?.onRecording ?? null;
  }

  get active(): boolean {
    return this.running;
  }

  /**
   * Acquire the microphone, open the talk WebSocket, and begin streaming.
   * Resolves once audio is flowing. Rejects on permission denial or a failed
   * (non-busy) handshake. A HTTP 429 from the server triggers onBusy and
   * resolves cleanly (no throw) so the UI can show a friendly message.
   */
  async start(url: string): Promise<void> {
    if (this.running) return;
    // Reset stop bookkeeping from a prior cycle so the next stop() actually
    // awaits this session's WebSocket close rather than returning a stale
    // resolved promise.
    this.stopping = false;
    this.stopPromise = null;
    this.resetRecording();

    // 1. Microphone (fail fast on permission denial before opening the socket)
    this.stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: true,
        noiseSuppression: true,
        autoGainControl: true,
      },
      video: false,
    });

    // 2. Open the WebSocket. The server opens the ALSA playback device before
    //    completing the handshake, so a 429 (busy) / 500 (device) surfaces as
    //    a failed upgrade here rather than mid-stream.
    try {
      await this.openSocket(url);
    } catch (err) {
      this.cleanup();
      throw err;
    }
    if (!this.ws) {
      // openSocket invoked onBusy and cleaned up.
      return;
    }

    this.running = true;

    // 3. AudioContext @ 48 kHz (browser resamples the mic to 48 kHz)
    this.audioCtx = new AudioContext({ sampleRate: SAMPLE_RATE });
    if (this.audioCtx.state === 'suspended') {
      this.audioCtx.resume().catch(() => {});
    }
    this.source = this.audioCtx.createMediaStreamSource(this.stream);

    // 4. Prefer AudioWorklet; fall back to ScriptProcessorNode.
    let useWorklet = false;
    try {
      useWorklet = await this.initWorklet();
    } catch (err) {
      console.error('[AudioTalker] worklet init failed, falling back:', err);
      useWorklet = false;
    }
    if (!useWorklet) {
      this.initFallback();
    }
  }

  /**
   * Stop capturing and release every resource. Resolves once the WebSocket has
   * fully closed (or after a 500 ms grace timeout), so the caller can safely
   * issue a follow-up SetAudioConfig({mute:false}) WITHOUT racing the
   * server-side StreamAudioPcm teardown that happens on WS close. Without this
   * serialization, the unmute can arrive while camera-daemon is still draining
   * the playback PCM and crash it. Safe to call multiple times (idempotent).
   */
  stop(): Promise<void> {
    if (this.stopping) return this.stopPromise ?? Promise.resolve();
    this.stopping = true;
    this.running = false;
    const { ws } = this;
    this.stopPromise = AudioTalker.closeSocketAsync(ws).finally(() => {
      this.emitRecording();
      this.cleanup();
    });
    return this.stopPromise;
  }

  /**
   * Await the WebSocket's actual close. The platform-api talk handler tears
   * down the gRPC StreamAudioPcm stream in its WS onclose, so the device-side
   * stop_playback only happens after this resolves. Capped at 500 ms so a hung
   * socket never blocks the UI thread.
   */
  private static closeSocketAsync(ws: WebSocket | null): Promise<void> {
    if (!ws) return Promise.resolve();
    if (
      ws.readyState !== WebSocket.OPEN
      && ws.readyState !== WebSocket.CONNECTING
    ) {
      return Promise.resolve();
    }
    return new Promise<void>(resolve => {
      const timer = setTimeout(() => resolve(), 500);
      // Replace the openSocket onclose (error-reporting path) — a user-initiated
      // stop is expected, so no error callback should fire. cleanup() will null
      // this handler after we resolve.
      ws.onclose = () => {
        clearTimeout(timer);
        resolve();
      };
      try {
        ws.close();
      } catch {
        clearTimeout(timer);
        resolve();
      }
    });
  }

  // ---- AudioWorklet path (preferred) ----

  private async initWorklet(): Promise<boolean> {
    if (!this.audioCtx?.audioWorklet) return false;

    await this.audioCtx.audioWorklet.addModule('/mic-processor.js');

    this.workletNode = new AudioWorkletNode(this.audioCtx, 'mic-processor');

    this.workletNode.port.onmessage = (e: MessageEvent) => {
      const data = e.data as { buffer?: ArrayBuffer; level?: number } | null;
      if (!data) return;
      if (typeof data.level === 'number' && this.onLevelCb) {
        this.onLevelCb(data.level);
      }
      if (data.buffer && this.ws && this.ws.readyState === WebSocket.OPEN) {
        this.recordPcm(data.buffer);
        this.ws.send(data.buffer);
      }
    };

    this.source?.connect(this.workletNode);
    return true;
  }

  // ---- ScriptProcessorNode fallback ----

  private initFallback(): void {
    if (!this.audioCtx) return;

    this.accumPos = 0;
    this.processor = this.audioCtx.createScriptProcessor(
      FALLBACK_BUFFER_SIZE,
      1,
      0 // capture only — no output
    );
    this.processor.onaudioprocess = e => this.onCaptureProcess(e);

    this.source?.connect(this.processor);
    // ScriptProcessorNode still needs a destination connection to run its
    // event loop on some engines; route through a zero-gain node to stay silent.
    const silent = this.audioCtx.createGain();
    silent.gain.value = 0;
    this.processor.connect(silent);
    silent.connect(this.audioCtx.destination);
  }

  private onCaptureProcess(e: AudioProcessingEvent): void {
    const input = e.inputBuffer.getChannelData(0);
    for (let i = 0; i < input.length; i++) {
      this.accum[this.accumPos++] = input[i];
      if (this.accumPos >= this.accum.length) {
        this.flushFallback();
      }
    }
  }

  private flushFallback(): void {
    const len = this.accumPos;
    this.accumPos = 0;

    let sumSq = 0;
    const buf = new ArrayBuffer(len * 2);
    const view = new DataView(buf);
    for (let i = 0; i < len; i++) {
      let s = this.accum[i];
      if (s > 1) s = 1;
      else if (s < -1) s = -1;
      view.setInt16(i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
      sumSq += s * s;
    }
    if (this.onLevelCb) {
      this.onLevelCb(Math.sqrt(sumSq / Math.max(1, len)));
    }
    if (this.ws && this.ws.readyState === WebSocket.OPEN) {
      this.recordPcm(buf);
      this.ws.send(buf);
    }
  }

  private recordPcm(buf: ArrayBuffer): void {
    if (buf.byteLength === 0) return;
    this.recordingChunks.push(buf.slice(0));
    this.recordingBytes += buf.byteLength;
  }

  private resetRecording(): void {
    this.recordingChunks = [];
    this.recordingBytes = 0;
  }

  private emitRecording(): void {
    if (!this.onRecordingCb || this.recordingBytes === 0) {
      this.resetRecording();
      return;
    }

    const durationMs = Math.round(
      (this.recordingBytes / (SAMPLE_RATE * CHANNELS * BYTES_PER_SAMPLE)) * 1000
    );
    const blob = createWavBlob(
      this.recordingChunks,
      this.recordingBytes,
      SAMPLE_RATE,
      CHANNELS
    );

    this.onRecordingCb({
      blob,
      durationMs,
      pcmBytes: this.recordingBytes,
      sampleRate: SAMPLE_RATE,
      channels: CHANNELS,
    });
    this.resetRecording();
  }

  // ---- Connection ----

  private openSocket(url: string): Promise<void> {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(url);
      ws.binaryType = 'arraybuffer';

      ws.onopen = () => {
        this.ws = ws;
        resolve();
      };

      ws.onerror = () => {
        // onclose will follow with detail.
      };

      ws.onclose = (ev: CloseEvent) => {
        const wasOpen = this.ws === ws;
        if (!this.ws) {
          // Handshake never completed → classify by close code/reason.
          if (ev.code === 1013 || /429|busy/i.test(ev.reason)) {
            this.busyDetected();
            resolve();
            return;
          }
          reject(
            new Error(
              `talk WebSocket closed before open (${ev.code} ${ev.reason})`
            )
          );
          return;
        }
        if (wasOpen && this.running && this.onErrorCb) {
          this.onErrorCb(`talk stream closed (${ev.code} ${ev.reason})`);
        }
      };
    });
  }

  private busyDetected(): void {
    this.running = false;
    if (this.onBusyCb) this.onBusyCb();
    this.cleanup();
  }

  // ---- Cleanup ----

  private cleanupAudio(): void {
    if (this.workletNode) {
      this.workletNode.port.onmessage = null;
      this.workletNode.disconnect();
      this.workletNode = null;
    }
    if (this.processor) {
      this.processor.onaudioprocess = null;
      this.processor.disconnect();
      this.processor = null;
    }
    if (this.source) {
      this.source.disconnect();
      this.source = null;
    }
    if (this.stream) {
      for (const track of this.stream.getTracks()) {
        track.stop();
      }
      this.stream = null;
    }
    if (this.audioCtx) {
      this.audioCtx.close().catch(() => {});
      this.audioCtx = null;
    }
  }

  private cleanup(): void {
    if (this.ws) {
      this.ws.onopen = null;
      this.ws.onerror = null;
      this.ws.onclose = null;
      if (
        this.ws.readyState === WebSocket.OPEN
        || this.ws.readyState === WebSocket.CONNECTING
      ) {
        try {
          this.ws.close();
        } catch {
          /* ignore */
        }
      }
      this.ws = null;
    }
    this.cleanupAudio();
  }
}
