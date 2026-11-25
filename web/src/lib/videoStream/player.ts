import { CTPlayer } from '@/lib/ct-player/core/player';
import { createBuiltinAdapter } from '@/lib/ct-player/adapters/registry';
import type { SourceAdapter } from '@/lib/ct-player/shared/types';
import type { StreamPlayerOptions, StreamPlayerStats } from './types';

// Device protocol (V2): every video message carries a fixed 18-byte private
// header that must be stripped before decoding; PTS is a big-endian 90 kHz
// value at byte offset 6.
export function deviceAdapter(): SourceAdapter {
  // 'avcc' is a registry builtin, so creation never yields null here.
  const adapter = createBuiltinAdapter({
    name: 'avcc',
    options: { headerBytes: 18, ptsOffset: 6 },
  })!;
  return {
    name: 'neoruntime',
    push(message) {
      if (message.length === 0) return [];
      return adapter.push(message);
    },
    reset() { adapter.reset(); },
  };
}

export class VideoStreamPlayer {
  private player: CTPlayer | null = null;

  private video: HTMLVideoElement | null = null;

  private url = '';

  private started = false;

  private waitingForData = true;

  private snapshotWidth?: number;

  private snapshotHeight?: number;

  private readonly options: StreamPlayerOptions;

  private objectFit?: 'contain' | 'cover';

  private frameWidth = 0;

  private frameHeight = 0;

  constructor(options: StreamPlayerOptions = {}) {
    if (options.protocol && options.protocol !== 'mse-h264') {
      throw new Error(`Unsupported stream protocol: ${options.protocol}`);
    }
    this.options = options;
    this.snapshotWidth = options.snapshotWidth;
    this.snapshotHeight = options.snapshotHeight;
    this.objectFit = options.objectFit;
  }

  initPlayer(video: HTMLVideoElement): this { this.video = video; return this; }

  get currentTime(): number { return this.video?.currentTime ?? 0; }

  get packetsPerSecond(): number { return this.player?.getStats().pps ?? 0; }

  get packetCount(): number { return this.packetsPerSecond; }

  resetStartState(): this { this.started = false; return this; }

  /** 运行时更新渲染填充模式（adaptive 由宿主解析后传入，引擎下一帧生效）。 */
  setObjectFit(fit: 'contain' | 'cover'): void {
    this.objectFit = fit;
    this.player?.setObjectFit(fit);
  }

  waitForNextVideoData(): void {
    this.waitingForData = true;
    window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
  }

  async start(url: string): Promise<this> {
    if (this.started && this.url === url) return this;
    if (!this.video) throw new Error('Call initPlayer before start');
    this.player?.destroy();
    this.video.dispatchEvent(new CustomEvent('aipc:software-hevc', { detail: false }));
    this.started = true;
    this.url = url;
    this.waitForNextVideoData();
    const player = new CTPlayer({
      objectFit: this.objectFit,
      wasm: { kernelBaseUrl: new URL(`${import.meta.env.BASE_URL}ct-player/wasm-kernel/`, window.location.origin).href },
    }).attach(this.video);
    this.player = player;
    let softwareHevc = false;
    let hasStartedPlaying = false;
    // Set when the engine's request-stream fallback has been routed to the
    // codec_mismatch event; the declined engine's terminal error is redundant.
    let streamFallbackRouted = false;
    const markWorking = () => {
      if (!this.waitingForData) return;
      this.waitingForData = false;
      window.dispatchEvent(new CustomEvent('wv_work', { detail: true }));
    };
    player.on('state', ({ state }) => {
      if (this.player !== player) return;
      if (state === 'buffering') window.dispatchEvent(new CustomEvent('wv_open'));
      if (state === 'connecting' || state === 'degraded') this.waitForNextVideoData();
      if (state === 'playing') {
        hasStartedPlaying = true;
        markWorking();
      }
    });
    player.on('stats', stats => {
      if (this.player !== player) return;
      const nextSoftwareHevc = stats.codec === 'h265' && stats.tech === 'wasm';
      if (nextSoftwareHevc !== softwareHevc) {
        softwareHevc = nextSoftwareHevc;
        this.video?.dispatchEvent(new CustomEvent('aipc:software-hevc', { detail: softwareHevc }));
      }
      // Canvas pipelines keep the <video> element empty, so publish the
      // decoded frame size here for consumers (overlay geometry, snapshot
      // dimensions, adaptive object-fit) that otherwise read videoWidth.
      if (
        stats.videoWidth && stats.videoHeight
        && (stats.videoWidth !== this.frameWidth || stats.videoHeight !== this.frameHeight)
      ) {
        this.frameWidth = stats.videoWidth;
        this.frameHeight = stats.videoHeight;
        this.video?.dispatchEvent(new CustomEvent('aipc:video-frame-size', {
          detail: { width: stats.videoWidth, height: stats.videoHeight },
        }));
      }
      if (hasStartedPlaying && stats.pps > 0) markWorking();
    });
    player.on('error', error => {
      if (this.player !== player) return;
      // Terminal transport failures must surface the disconnected UI: the
      // shared Player ignores plain wv_error events, so translate them into
      // wv_close instead.
      if (error.code === 'RETRY_EXHAUSTED' || error.code === 'SUPERSEDED') {
        window.dispatchEvent(new CustomEvent('wv_close', { detail: error.detail }));
        return;
      }
      if (error.code === 'NETWORK_ERROR') {
        // Socket closes carry {code, reason} and are already beyond the
        // reconnect policy — stay disconnected instead of re-entering
        // buffering with no reconnect scheduled. Other transport errors
        // (string detail) keep retrying and recover through state/stats.
        if (error.detail && typeof error.detail === 'object') {
          window.dispatchEvent(new CustomEvent('wv_close', { detail: error.detail }));
        }
        return;
      }
      if (error.fatal) {
        if (streamFallbackRouted) return;
        const codecMismatch = error.code === 'CODEC_MISMATCH' || error.code === 'CODEC_UNSUPPORTED';
        window.dispatchEvent(new CustomEvent('wv_error', {
          detail: codecMismatch ? { type: 'codec_mismatch', codec: player.getStats().codec, message: error.code } : error,
        }));
        this.options.onEvent?.({ t: codecMismatch ? 'codecMismatch' : error.code });
      }
    });
    player.on('request-stream', request => {
      if (this.player !== player) return;
      // The engine exhausted its tech chain for HEVC (hardware + WASM) and
      // asks for an H.264 stream. Route the request through the existing
      // codec_mismatch fallback (useStreamFallback swaps the stream URL and
      // rebuilds this facade), and decline so the old engine terminates
      // instead of waiting on a stream that will never arrive.
      streamFallbackRouted = true;
      window.dispatchEvent(new CustomEvent('wv_error', {
        detail: { type: 'codec_mismatch', codec: request.current.codec, message: request.reason },
      }));
      player.respondStreamChange({ accepted: false });
    });
    try {
      await player.play({
        transport: { type: 'ws', url },
        adapter: async () => deviceAdapter(),
      });
    } catch (error) {
      if (this.player === player) {
        this.started = false;
        window.dispatchEvent(new CustomEvent('wv_error', { detail: error }));
      }
    }
    return this;
  }

  pause(): void {
    this.started = false;
    this.player?.destroy();
    this.player = null;
  }

  async reStart(): Promise<void> {
    if (!this.url || !this.video) return;
    this.started = false;
    await this.start(this.url);
  }

  destroy(): void {
    const { player } = this;
    this.player = null;
    this.started = false;
    player?.destroy();
    this.video = null;
    this.url = '';
  }

  getStats(): StreamPlayerStats | null {
    if (!this.player) return null;
    const stats = this.player.getStats();
    return {
      bandwidth: stats.bitrateKBps,
      // stats.latencyMs is milliseconds; the consumer renders seconds.
      latency: (stats.latencyMs ?? 0) / 1000,
      bufferTime: (stats.bufferedMs ?? 0) / 1000,
      playbackRate: this.video?.playbackRate ?? 1,
      packetsPerSecond: stats.pps,
    };
  }

  setSnapshotDimensions(width: number, height: number): void {
    this.snapshotWidth = width;
    this.snapshotHeight = height;
  }

  doSnapshot(): void {
    const { player } = this;
    if (!player) return;
    player.snapshot().then(blob => {
      const download = (image: Blob) => {
        const url = URL.createObjectURL(image);
        const link = document.createElement('a');
        link.href = url;
        link.download = `snapshot-${Date.now()}.png`;
        link.click();
        URL.revokeObjectURL(url);
      };
      if (!this.snapshotWidth || !this.snapshotHeight) { download(blob); return; }
      const source = URL.createObjectURL(blob);
      const image = new Image();
      image.onload = () => {
        const canvas = document.createElement('canvas');
        canvas.width = this.snapshotWidth!;
        canvas.height = this.snapshotHeight!;
        const ctx = canvas.getContext('2d');
        ctx?.drawImage(image, 0, 0, Math.min(image.width, canvas.width), Math.min(image.height, canvas.height), 0, 0, canvas.width, canvas.height);
        canvas.toBlob(result => { if (result) download(result); });
        URL.revokeObjectURL(source);
      };
      image.onerror = () => URL.revokeObjectURL(source);
      image.src = source;
    }).catch(error => console.warn('[VideoStreamPlayer] Snapshot unavailable', error));
  }
}

export type { StreamPlayerOptions, StreamPlayerStats, StreamProtocol } from './types';
