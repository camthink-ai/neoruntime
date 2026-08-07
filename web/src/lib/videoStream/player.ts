import H264Player from './MSE/msePlayer';
import { sleep } from '@/utils/index.js';
import type {
  StreamPlayerOptions,
  StreamPlayerStats,
  StreamProtocol,
} from './types';

interface WsWorkerMessage {
  type: 'connecting' | 'open' | 'close' | 'error' | 'video-data';
  error?: string;
  payload?: ArrayBuffer;
  code?: number;
  reason?: string;
}

/**
 * 协议无关的统一播放器封装
 *
 * - 当前仅支持 `mse-h264`，内部复用原来的 `H264Player`（msePlayer）做解码
 * - WebSocket 连接 / 断开、重连、事件派发全部提升到这一层
 * - 之后接入 webrtc / hls 时，只需要在这里扩展协议分发逻辑
 */
export class VideoStreamPlayer {
  private readonly protocol: StreamProtocol;

  // 目前的具体实现：MSE + H264
  private mseH264Player: H264Player | null = null;

  private webSocketWorker: Worker | null = null;

  private wsUrl = '';

  private isStarted = false;

  private isConnected = false;

  private hasReceivedVideoData = false;

  private videoElement: HTMLVideoElement | null = null;

  // Flag to prevent processing data after destroy
  private isDestroyed = false;

  private mseErrorCount = 0;

  private lastMseErrorTime = 0;

  private mseReconnecting = false;

  // Stall detection
  private stallCheckTimer: ReturnType<typeof setInterval> | null = null;

  private stallZeroCount = 0;

  private stallRecoveryCount = 0;

  private static readonly STALL_CHECK_INTERVAL = 1000;

  private static readonly STALL_ZERO_THRESHOLD = 3;

  private static readonly STALL_MAX_RECOVERY = 3;

  private snapshotWidth?: number;

  private snapshotHeight?: number;

  constructor(options: StreamPlayerOptions = {}) {
    this.protocol = options.protocol ?? 'mse-h264';
    this.snapshotWidth = options.snapshotWidth;
    this.snapshotHeight = options.snapshotHeight;

    const wrappedOnEvent = (msg: { t: string; codec?: string }) => {
      if (msg.t === 'mseError') {
        this.handleMseError();
      } else if (msg.t === 'codecMismatch') {
        // Codec not supported (e.g. H265) — stop player, don't reconnect
        console.error(
          `[VideoStreamPlayer] Codec mismatch: ${msg.codec} not supported`
        );
        this.stopStallDetection();
        this.wsDisconnect();
        this.mseH264Player?.stopPlay();
        window.dispatchEvent(
          new CustomEvent('wv_error', {
            detail: {
              type: 'codec_mismatch',
              codec: msg.codec,
              message: `${msg.codec} is not supported`,
            },
          })
        );
      }
      options.onEvent?.(msg);
    };

    switch (this.protocol) {
      case 'mse-h264':
        this.mseH264Player = new H264Player(wrappedOnEvent);
        break;
      default:
        throw new Error(`Unsupported stream protocol: ${this.protocol}`);
    }
  }

  private handleMseError(): void {
    const now = Date.now();
    if (now - this.lastMseErrorTime > 10_000) {
      this.mseErrorCount = 0;
    }
    this.mseErrorCount++;
    this.lastMseErrorTime = now;

    if (this.mseErrorCount >= 3 && !this.mseReconnecting) {
      this.mseReconnecting = true;
      this.mseErrorCount = 0;
      this.reStart().finally(() => {
        this.mseReconnecting = false;
      });
    }
  }

  private startStallDetection(): void {
    this.stopStallDetection();
    this.stallZeroCount = 0;
    this.stallCheckTimer = setInterval(() => {
      if (this.isDestroyed || !this.isConnected) return;
      if (this.packetsPerSecond === 0) {
        this.stallZeroCount++;
        if (
          this.stallZeroCount >= VideoStreamPlayer.STALL_ZERO_THRESHOLD
          && this.stallRecoveryCount < VideoStreamPlayer.STALL_MAX_RECOVERY
        ) {
          this.stallRecoveryCount++;
          console.warn(
            `[VideoStreamPlayer] Stream stall detected (0 pps for ${this.stallZeroCount}s), restarting (attempt ${this.stallRecoveryCount}/${VideoStreamPlayer.STALL_MAX_RECOVERY})`
          );
          this.stallZeroCount = 0;
          this.reStart().catch(() => {});
        }
      } else {
        this.stallZeroCount = 0;
        this.stallRecoveryCount = 0;
      }
    }, VideoStreamPlayer.STALL_CHECK_INTERVAL);
  }

  private stopStallDetection(): void {
    if (this.stallCheckTimer) {
      clearInterval(this.stallCheckTimer);
      this.stallCheckTimer = null;
    }
    this.stallZeroCount = 0;
  }

  /**
   * 初始化 video 元素
   */
  initPlayer(videoElement: HTMLVideoElement): this {
    this.videoElement = videoElement;

    switch (this.protocol) {
      case 'mse-h264':
        this.mseH264Player?.initPlayer(videoElement);
        break;
      default:
        break;
    }
    return this;
  }

  /**
   * 外部可读的时间与统计信息
   */
  get currentTime(): number {
    return this.mseH264Player?.currentTime ?? 0;
  }

  get packetsPerSecond(): number {
    return this.mseH264Player?.packetsPerSecond ?? 0;
  }

  get packetCount(): number {
    return this.mseH264Player?.packetCount ?? 0;
  }

  /**
   * 内部：初始化 WS Worker & 解码 Worker
   */
  private initWorkers(): void {
    if (this.protocol !== 'mse-h264') return;

    if (!this.mseH264Player) return;

    // 初始化解码 Worker（仅一次）
    this.mseH264Player.initDecoder();

    if (!this.webSocketWorker) {
      // 启动连接前通知外部展示 loading
      window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
      this.webSocketWorker = new Worker(new URL('./ws.ts', import.meta.url), {
        type: 'module',
      });
      this.setupWebSocketWorkerHandler();
    }
  }

  /**
   * 绑定 WS Worker 消息回调
   */
  private setupWebSocketWorkerHandler(): void {
    if (!this.webSocketWorker) return;

    this.webSocketWorker.onmessage = (ev: MessageEvent<WsWorkerMessage>) => {
      const msg = ev.data;
      if (!msg || typeof msg !== 'object') return;

      switch (msg.type) {
        case 'connecting':
          this.hasReceivedVideoData = false;
          // 连接建立中：展示 loading
          window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
          break;
        case 'open':
          this.isConnected = true;
          // A connected socket may still have no stream data. Keep loading
          // until the first non-empty video payload arrives.
          window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
          window.dispatchEvent(new CustomEvent('wv_open'));
          break;
        case 'close':
          this.isConnected = false;
          this.hasReceivedVideoData = false;
          window.dispatchEvent(
            new CustomEvent('wv_close', {
              detail: { code: msg.code, reason: msg.reason },
            })
          );
          break;
        case 'error':
          this.isConnected = false;
          this.hasReceivedVideoData = false;
          window.dispatchEvent(
            new CustomEvent('wv_error', { detail: msg.error })
          );
          // 出错期间保持 loading
          window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
          break;
        case 'video-data': {
          // Skip processing if player is destroyed
          if (this.isDestroyed) {
            // console.warn('[VideoStreamPlayer] Ignoring video-data, player is destroyed');
            return;
          }

          if (msg.payload instanceof ArrayBuffer) {
            // 将 WS 数据交给底层解码器
            this.mseH264Player?.handleVideoData(msg.payload);
            if (!this.hasReceivedVideoData && msg.payload.byteLength > 0) {
              this.hasReceivedVideoData = true;
              window.dispatchEvent(
                new CustomEvent('wv_work', { detail: true })
              );
            }
          }
          break;
        }
        default:
          break;
      }
    };
  }

  private wsDisconnect(): void {
    this.isConnected = false;
    this.hasReceivedVideoData = false;
    this.webSocketWorker?.postMessage({ type: 'disconnect' });
  }

  private wsConnect(url: string): void {
    this.webSocketWorker?.postMessage({ type: 'connect', url });
  }

  private async shutdownWebSocketWorker(graceMs = 250): Promise<void> {
    const worker = this.webSocketWorker;
    if (!worker) return;

    this.isConnected = false;
    this.hasReceivedVideoData = false;
    this.webSocketWorker = null;
    try {
      worker.postMessage({ type: 'disconnect' });
    } catch {
      // ignore
    }
    await sleep(graceMs);
    try {
      worker.terminate();
    } catch {
      // ignore
    }
  }

  private releaseWebSocketWorker(graceMs = 250): void {
    const worker = this.webSocketWorker;
    if (!worker) return;

    this.isConnected = false;
    this.hasReceivedVideoData = false;
    this.webSocketWorker = null;
    try {
      worker.postMessage({ type: 'disconnect' });
    } catch {
      // ignore
    }
    setTimeout(() => {
      try {
        worker.terminate();
      } catch {
        // ignore
      }
    }, graceMs);
  }

  resetStartState(): this {
    this.isStarted = false;
    return this;
  }

  // Force readiness to wait for a payload newer than the current one.
  waitForNextVideoData(): void {
    this.hasReceivedVideoData = false;
    window.dispatchEvent(new CustomEvent('wv_work', { detail: false }));
  }

  /**
   * 开始播放（由上层传入 WS URL）
   */
  async start(url: string): Promise<this> {
    const sameUrl = this.wsUrl === url;

    // Prevent starting if already started with the same URL
    if (this.isStarted && sameUrl && !this.isDestroyed) {
      return this;
    }

    this.wsUrl = url;

    // Reset destroyed flag when starting
    this.isDestroyed = false;
    this.hasReceivedVideoData = false;

    // Clean up existing connection first
    if (this.isConnected || this.webSocketWorker) {
      await this.shutdownWebSocketWorker();
    }

    this.isStarted = true;
    this.initWorkers();
    this.wsConnect(url);

    // 通知解码器进入新流
    this.mseH264Player?.initDecoderForNewStream();

    this.startStallDetection();

    return this;
  }

  /**
   * 暂停：断开 WS，但保留 MSE 缓冲和 video 元素
   */
  pause(): void {
    this.isConnected = false;
    this.hasReceivedVideoData = false;
    this.isStarted = false;
    this.stopStallDetection();

    if (this.webSocketWorker) {
      this.releaseWebSocketWorker();
    }

    // 仅暂停视频播放 & 清缓冲
    this.mseH264Player?.clearBuffer();
    if (this.videoElement && !this.videoElement.paused) {
      this.videoElement.pause();
    }
  }

  /**
   * 重启流：清空缓冲并重新建立 WS 连接
   */
  async reStart(): Promise<void> {
    if (!this.wsUrl || !this.videoElement) return;

    // Reset destroyed flag when restarting
    this.isDestroyed = false;
    this.hasReceivedVideoData = false;

    if (this.isConnected && this.webSocketWorker) {
      this.wsDisconnect();
      await sleep(500);
    }

    this.initWorkers();

    // 重新关联 video 元素
    if (this.mseH264Player && this.videoElement) {
      this.mseH264Player.initPlayer(this.videoElement);
    }

    // 清空 MSE buffer
    this.mseH264Player?.clearBuffer();

    // 重连 WS
    this.wsConnect(this.wsUrl);

    // 重新初始化解码器
    this.mseH264Player?.initDecoderForNewStream();

    this.isStarted = true;
  }

  /**
   * 彻底销毁播放器实例
   */
  destroy(): void {
    // Set flag immediately to prevent processing any remaining WebSocket messages
    this.isDestroyed = true;
    this.isStarted = false;
    this.isConnected = false;
    this.hasReceivedVideoData = false;
    this.stopStallDetection();

    if (this.webSocketWorker) {
      this.releaseWebSocketWorker();
    }

    this.mseH264Player?.stopPlay();
    this.mseH264Player?.destroy();
    this.mseH264Player = null;

    this.videoElement = null;
    this.wsUrl = '';
  }

  /**
   * 获取底层实现暴露的统计指标
   */
  getStats(): StreamPlayerStats | null {
    switch (this.protocol) {
      case 'mse-h264':
        return this.mseH264Player?.getStats() ?? null;
      default:
        return null;
    }
  }

  /**
   * Update snapshot dimensions (e.g. after media config loads or changes)
   */
  setSnapshotDimensions(width: number, height: number): void {
    this.snapshotWidth = width;
    this.snapshotHeight = height;
  }

  /**
   * 截图当前帧
   */
  doSnapshot(): void {
    const videoEl = this.videoElement;
    if (!videoEl) return;

    const canvas = document.createElement('canvas');
    const targetW = this.snapshotWidth || videoEl.videoWidth || videoEl.clientWidth;
    const targetH = this.snapshotHeight || videoEl.videoHeight || videoEl.clientHeight;
    canvas.width = targetW;
    canvas.height = targetH;

    const ctx = canvas.getContext('2d');
    if (!ctx) return;

    // Crop source to configured dimensions to remove macroblock padding
    // (e.g. 1088→1080, 368→360) instead of stretching
    const srcH = Math.min(targetH, videoEl.videoHeight);
    ctx.drawImage(videoEl, 0, 0, targetW, srcH, 0, 0, targetW, targetH);

    canvas.toBlob(blob => {
      if (!blob) return;
      const url = URL.createObjectURL(blob);
      const a = document.createElement('a');
      a.href = url;
      a.download = `snapshot-${Date.now()}.png`;
      a.click();
      URL.revokeObjectURL(url);
    });
  }
}

export type {
  StreamPlayerOptions,
  StreamPlayerStats,
  StreamProtocol,
} from './types';
