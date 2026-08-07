// Type definitions
interface FrameData {
  data: ArrayBuffer;
  codec?: string;
}

interface CallbackEvent {
  t: 'mseError' | 'startPlay';
}

type CallbackFunction = (event: CallbackEvent) => void;

interface Mp4EventData {
  data: ArrayBuffer;
  codec: string;
}

type MediaSourceConstructor = {
  new (): MediaSource;
  isTypeSupported?(codec: string): boolean;
};
declare global {
  interface Window {
    ManagedMediaSource?: MediaSourceConstructor;
  }
}
class MsMediaSource {
  private mediaSource: MediaSource | null = null;

  private videoElement: HTMLVideoElement | null = null;

  private sourceBuffer: SourceBuffer | null = null;

  private frameBuffer: FrameData[] = [];

  private updateend: number = 1;

  private mimeCodec: string = '';

  private initFlag: number = 0;

  private cb: CallbackFunction;

  private currentSegmentIndex: number = 0;

  private isPlayback: boolean = false; // false: preview, true: playback

  // Debounced seek: only seek when lag is large, with cooldown to prevent oscillation

  // Buffer cleanup timer (avoids remove() inside updateend cycle)
  private cleanupTimerId: ReturnType<typeof setInterval> | null = null;

  // Bound callback reference for proper event listener removal
  private boundVideoErrorCallback: ((e: Event) => void) | null = null;

  // Buffer management optimization. The window kept behind the live edge is
  // intentionally generous: over a jittery link (e.g. a Wi-Fi camera) a small
  // buffer underruns on every burst/hole and surfaces as stutter. More buffer
  // costs memory + latency but yields a stable frame cadence.
  private readonly MAX_FRAME_BUFFER_SIZE: number = 30; // Limit frame buffer size (1 second at 30fps)

  private readonly BUFFER_WINDOW_SIZE: number = 3; // Keep ~3 seconds of buffer behind the live edge

  // Latency chase parameters. Tuned for SMOOTHNESS over jittery links: a wide
  // dead-band (0.6–1.5 s) lets the playhead settle at a steady buffer depth
  // instead of micro-toggling playbackRate 1.0↔1.05, which reads as constant
  // micro-stutter. The hard-seek threshold is high enough that a transient lag
  // spike (common on Wi-Fi) doesn't jump frames. This trades roughly one extra
  // second of latency for a visibly stable cadence.
  private static readonly LATENCY_TARGET: number = 0.6; // below this, normal rate (1.0)

  private static readonly LATENCY_CHASE_START: number = 1.5; // start gentle catchup above this

  private static readonly LATENCY_HARD_SEEK: number = 3.0; // hard-seek only on real stalls

  private static readonly CHASE_RATE: number = 1.05; // playback rate during catchup

  private static readonly SEEK_COOLDOWN_MS: number = 5000; // min interval between hard-seeks

  private lastSeekTime: number = 0;

  private lastDecodeErrorRecovery: number = 0;

  constructor(cb: CallbackFunction) {
    this.cb = cb;
    this.boundVideoErrorCallback = this.videoErrorCallback.bind(this);
  }

  static get statusIdel(): number {
    return 0;
  }

  static get statusWait(): number {
    return 1;
  }

  static get statusNormal(): number {
    return 2;
  }

  static get statusError(): number {
    return 3;
  }

  static get statusDestroy(): number {
    return 4;
  }

  // Latency control: DISABLED to prevent frame rollback
  // Auto-seek causes jumps even with large buffer
  // private static readonly SEEK_LAG_THRESHOLD: number = 5;
  // private static readonly SEEK_COOLDOWN_MS: number = 3000;
  // private static readonly SEEK_TARGET_OFFSET: number = 0.3;

  initMse(codec: string): boolean {
    // console.log('[MsMediaSource] initMse called, codec:', codec, 'videoElement:', this.videoElement ? 'exists' : 'NULL')
    const MediaSourceCtor = (window.ManagedMediaSource
      ?? window.MediaSource) as MediaSourceConstructor | undefined;
    if (!MediaSourceCtor) {
      console.error('[MsMediaSource] MediaSource not supported');
      return false;
    }

    this.mimeCodec = codec;

    try {
      // Clean up existing MSE state to prevent decoder corruption during stream switch
      if (this.sourceBuffer) {
        this.uninitSourceBuffer();
      }
      if (this.mediaSource) {
        try {
          if (this.mediaSource.readyState === 'open') {
            this.mediaSource.endOfStream();
          }
        } catch {
          // Already closed
        }
        this.mediaSource = null;
      }

      this.mediaSource = new MediaSourceCtor();
      const currentMs = this.mediaSource;

      if (this.videoElement) {
        // Remove old error listener BEFORE any src changes
        if (this.boundVideoErrorCallback) {
          this.videoElement.removeEventListener(
            'error',
            this.boundVideoErrorCallback
          );
        }

        if (
          this.videoElement.src
          && this.videoElement.src !== window.location.href
        ) {
          try {
            window.URL.revokeObjectURL(this.videoElement.src);
          } catch {
            // Ignore revoke errors
          }
        }

        // Set new src — this internally runs the load algorithm which
        // resets error state, decoder state, and readyState.
        this.videoElement.src = window.URL.createObjectURL(this.mediaSource);

        // Register error listener AFTER new src is set — prevents catching
        // stale error events queued from the previous session's cleanup.
        if (this.boundVideoErrorCallback) {
          this.videoElement.addEventListener(
            'error',
            this.boundVideoErrorCallback
          );
        }
      }

      this.mediaSource.addEventListener('sourceopen', () => {
        if (this.mediaSource !== currentMs) return;
        console.log(
          '[MsMediaSource] sourceopen event, readyState=',
          this.mediaSource.readyState
        );
        this.uninitSourceBuffer();
        this.initSourceBuffer();
        this.updateSourceBuffer();
      });

      this.mediaSource.addEventListener('sourceclose', () => {
        if (this.mediaSource !== currentMs) return;
        this.initFlag = MsMediaSource.statusIdel;
      });

      this.mediaSource.addEventListener('sourceended', () => {
        if (this.mediaSource !== currentMs) return;
        this.initFlag = MsMediaSource.statusIdel;
      });
    } catch {
      return false;
    }

    this.videoElement?.pause();
    return true;
  }

  videoErrorCallback(e: Event): void {
    try {
      const target = e.target as HTMLVideoElement;
      if (target?.error) {
        if (target.src === '' || !this.videoElement) {
          return;
        }

        switch (target.error.code) {
          case target.error.MEDIA_ERR_ABORTED:
            return;
          case target.error.MEDIA_ERR_NETWORK:
          case target.error.MEDIA_ERR_DECODE:
          case target.error.MEDIA_ERR_SRC_NOT_SUPPORTED:
          default:
            break;
        }

        if (
          target.error.code === target.error.MEDIA_ERR_DECODE
          && this.videoElement
        ) {
          const now = Date.now();
          if (now - this.lastDecodeErrorRecovery < 1000) {
            this.cb({ t: 'mseError' });
            return;
          }
          this.lastDecodeErrorRecovery = now;

          this.frameBuffer = [];
          this.uninitSourceBuffer();
          if (this.mediaSource) {
            try {
              if (this.mediaSource.readyState === 'open') {
                this.mediaSource.endOfStream();
              }
            } catch {
              /* already closed */
            }
          }
          this.mediaSource = null;
          this.sourceBuffer = null;
          this.updateend = 1;
          this.initFlag = MsMediaSource.statusIdel;
          this.cb({ t: 'mseError' });
          return;
        }
      }

      if (!this.videoElement || this.initFlag === MsMediaSource.statusDestroy) {
        return;
      }

      this.initFlag = MsMediaSource.statusDestroy;
      this.cb({ t: 'mseError' });
    } catch {
      // Ignore errors during cleanup
    }
  }

  static makeBuffer(buffer1: Uint8Array, buffer2: Uint8Array): Uint8Array {
    const tmp = new Uint8Array(buffer1.byteLength + buffer2.byteLength);
    tmp.set(new Uint8Array(buffer1), 0);
    tmp.set(new Uint8Array(buffer2), buffer1.byteLength);
    return tmp;
  }

  initSourceBuffer(): number {
    if (this.sourceBuffer !== null) {
      return -1;
    }

    if (!this.mediaSource) {
      return -1;
    }

    this.sourceBuffer = this.mediaSource.addSourceBuffer(this.mimeCodec);
    this.currentSegmentIndex = 0;
    const curMode = this.sourceBuffer.mode;
    if (curMode === 'segments') {
      this.sourceBuffer.mode = 'sequence';
    }

    // this.lastSeekTime = 0;  // reset handled by constructor default
    this.startCleanupTimer();

    this.sourceBuffer.addEventListener('updateend', () => {
      try {
        if (
          this.sourceBuffer !== null
          && this.mediaSource?.readyState === 'open'
          && this.videoElement
        ) {
          this.handleTimeUpdate();
          if (
            !this.sourceBuffer
            || !this.mediaSource
            || this.mediaSource.readyState !== 'open'
          ) return;

          let buffered;
          try {
            buffered = this.sourceBuffer.buffered;
          } catch {
            return;
          }

          if (buffered.length === 0) {
            this.updateend = 1;
            return;
          }
          // Clamp currentSegmentIndex
          if (this.currentSegmentIndex >= buffered.length) {
            this.currentSegmentIndex = buffered.length - 1;
          }

          // Latency chase for live preview:
          // - Gentle: playbackRate 1.05x when lag > 0.5s
          // - Hard:   seek to live edge when lag > 1.5s
          // - Dead zone 0.3–0.5s prevents oscillation
          if (!this.isPlayback && this.videoElement) {
            const lag =              buffered.end(buffered.length - 1) - this.videoElement.currentTime;
            const now = Date.now();
            if (
              lag > MsMediaSource.LATENCY_HARD_SEEK
              && now - this.lastSeekTime > MsMediaSource.SEEK_COOLDOWN_MS
            ) {
              // Hard seek to live edge — guard against backward seek
              const target = buffered.end(buffered.length - 1) - 0.1;
              if (target > this.videoElement.currentTime) {
                this.videoElement.currentTime = target;
              }
              this.videoElement.playbackRate = 1.0;
              this.lastSeekTime = now;
            } else if (lag > MsMediaSource.LATENCY_CHASE_START) {
              // Gentle catchup — barely noticeable at 1.05x
              this.videoElement.playbackRate = MsMediaSource.CHASE_RATE;
            } else if (lag <= MsMediaSource.LATENCY_TARGET) {
              // Caught up
              this.videoElement.playbackRate = 1.0;
            }
          }
        }
      } catch (e) {
        console.error('[MsMediaSource] updateend error:', e);
      }
      this.updateend = 1;
    });

    this.sourceBuffer.addEventListener('error', (e: Event) => {
      const sb = e.target as SourceBuffer & { error?: DOMException | null };
      const msg =        sb?.error?.message
        ?? (sb as unknown as { message?: string })?.message
        ?? '';
      console.error('[MsMediaSource] SourceBuffer error:', msg || e);

      if (this.initFlag === MsMediaSource.statusDestroy) {
        return;
      }

      try {
        this.frameBuffer = [];
        this.uninitSourceBuffer();
        if (this.mediaSource?.readyState === 'open') {
          try {
            this.mediaSource.endOfStream();
          } catch {
            /* ignore */
          }
        }
      } catch {
        /* ignore */
      }
      this.mediaSource = null;
      this.sourceBuffer = null;
      this.updateend = 1;
      this.initFlag = MsMediaSource.statusIdel;
      this.cb({ t: 'mseError' });
    });

    return 0;
  }

  handleTimeUpdate(): void {
    if (!this.sourceBuffer || !this.videoElement) return;

    try {
      const { buffered } = this.sourceBuffer;
      if (buffered.length > 1) {
        this.currentSegmentIndex = buffered.length - 1;
      }
    } catch {
      // Ignore if buffered is inaccessible
    }
  }

  /**
   * Periodic buffer cleanup — runs on a timer instead of inside updateend
   * to avoid remove() triggering recursive updateend cycles.
   */
  private startCleanupTimer(): void {
    this.stopCleanupTimer();
    this.cleanupTimerId = setInterval(() => {
      try {
        if (
          !this.sourceBuffer
          || this.sourceBuffer.updating
          || !this.mediaSource
          || this.mediaSource.readyState !== 'open'
          || !this.videoElement
        ) {
          return;
        }

        const { buffered } = this.sourceBuffer;
        if (buffered.length === 0) return;

        const bufferEnd = buffered.end(buffered.length - 1);
        const bufferStart = buffered.start(0);
        const { currentTime } = this.videoElement;
        const removeEnd = bufferEnd - this.BUFFER_WINDOW_SIZE;

        if (removeEnd > bufferStart && currentTime > bufferStart) {
          // Keep only 0.5 seconds before currentTime for minimal latency (all I-frame mode)
          const safeRemoveEnd = Math.min(removeEnd, currentTime - 0.5);
          if (safeRemoveEnd > bufferStart) {
            this.sourceBuffer.remove(bufferStart, safeRemoveEnd);

            if (
              this.mediaSource
              && 'setLiveSeekableRange' in this.mediaSource
            ) {
              try {
                (this.mediaSource as any).setLiveSeekableRange(
                  safeRemoveEnd,
                  bufferEnd
                );
              } catch {
                // Ignore if not supported
              }
            }
          }
        }
      } catch {
        // Ignore cleanup errors
      }
    }, 1000); // Run every 1 second for aggressive cleanup (low latency)
  }

  private stopCleanupTimer(): void {
    if (this.cleanupTimerId !== null) {
      clearInterval(this.cleanupTimerId);
      this.cleanupTimerId = null;
    }
  }

  uninitSourceBuffer(): void {
    this.stopCleanupTimer();

    if (this.sourceBuffer === null || !this.mediaSource) {
      return;
    }

    // Set flag first to prevent concurrent operations
    this.initFlag = MsMediaSource.statusDestroy;

    // this.sourceBuffer.removeEventListener("updateend", this.removeUpdateCallback);
    try {
      for (let i = 0; i < this.mediaSource.sourceBuffers.length; i++) {
        this.mediaSource.removeSourceBuffer(this.mediaSource.sourceBuffers[i]);
      }
    } catch {
      // ignore
    }
    this.sourceBuffer = null;
  }

  updateSourceBuffer(): void {
    if (this.sourceBuffer === null || this.updateend !== 1) {
      return;
    }
    try {
      if (this.sourceBuffer.updating) return;
    } catch {
      // SourceBuffer may have been removed from MediaSource
      return;
    }

    // Check if the SourceBuffer has been removed from MediaSource
    if (
      this.initFlag === MsMediaSource.statusDestroy
      || !this.mediaSource
      || this.mediaSource.readyState === 'closed'
    ) {
      return;
    }

    // Check if video element is in error state - don't append if it is
    if (this.videoElement?.error) {
      return;
    }

    const len = this.frameBuffer.length;
    if (len === 0) {
      return;
    }

    // Calculate total size first, then allocate once
    let totalSize = 0;
    for (let i = 0; i < len; i++) {
      totalSize += this.frameBuffer[i].data.byteLength;
    }

    // Debug log
    if (this.frameBuffer.length > 0) {
      // console.log('[MsMediaSource] Flushing', len, 'frames, total size:', totalSize)
    }

    // Allocate buffer once instead of repeatedly
    const segmentBuffer = new Uint8Array(totalSize);
    let offset = 0;

    // Copy all frames in one pass
    for (let i = 0; i < len; i++) {
      const frameData = new Uint8Array(this.frameBuffer[i].data);
      segmentBuffer.set(frameData, offset);
      offset += frameData.byteLength;
    }

    // Clear buffer after copying (more efficient than repeated shift())
    this.frameBuffer = [];

    try {
      this.sourceBuffer.appendBuffer(segmentBuffer);
      this.updateend = 0;
      if (this.videoElement?.paused) {
        this.videoElement.style.display = '';
        const playPromise = this.videoElement.play();
        if (playPromise) {
          playPromise
            .then(() => {
              this.cb({ t: 'startPlay' });
            })
            .catch(() => {
              // Browser rejected play() — typically Chrome pausing
              // video-only media to save power. Will retry on next
              // appendBuffer or when the tab regains visibility.
            });
        }
      }
    } catch {
      this.initFlag = MsMediaSource.statusDestroy;
      this.cb({
        t: 'mseError',
      });
    }
  }

  processMp4VideoData(
    event: { data: Mp4EventData },
    snapshotFlag: number
  ): void {
    const objData = event.data;

    if (this.initFlag === MsMediaSource.statusDestroy) {
      return;
    }

    // Allow init even when video.error is set from previous session —
    // initMse sets a new src which clears the stale error state.
    if (this.initFlag === MsMediaSource.statusIdel) {
      this.frameBuffer = [];
      this.initFlag = MsMediaSource.statusWait;
      if (this.initMse(objData.codec)) {
        this.initFlag = MsMediaSource.statusNormal;
      } else {
        console.error('[MsMediaSource] MSE initialization failed');
        this.initFlag = MsMediaSource.statusError;
      }
    }

    // Check error AFTER init phase (initMse clears stale errors by setting new src)
    if (this.videoElement?.error) {
      return;
    }

    // When the tab is hidden, Chrome pauses video-only media to save power.
    // Continuing to feed the SourceBuffer causes massive buffer bloat and
    // MEDIA_ERR_DECODE when the tab returns. Drop non-init data while hidden.
    if (document.hidden && this.initFlag === MsMediaSource.statusNormal) {
      return;
    }

    if (this.frameBuffer.length >= this.MAX_FRAME_BUFFER_SIZE) {
      this.frameBuffer.splice(0, Math.floor(this.MAX_FRAME_BUFFER_SIZE * 0.3));
    }

    this.frameBuffer.push(objData);
    if (snapshotFlag === 0) {
      this.updateSourceBuffer();
    }
  }

  processMp4AudioData(event: { data: Mp4EventData }): void {
    const objData = event.data;

    if (this.initFlag === MsMediaSource.statusIdel) {
      this.frameBuffer = [];
      this.initFlag = MsMediaSource.statusWait;
      if (this.initMse(objData.codec)) {
        this.initFlag = MsMediaSource.statusNormal;
      } else {
        this.initFlag = MsMediaSource.statusError;
      }
    }

    // Buffer size limit for audio as well
    if (this.frameBuffer.length >= this.MAX_FRAME_BUFFER_SIZE) {
      this.frameBuffer.splice(0, Math.floor(this.MAX_FRAME_BUFFER_SIZE * 0.3));
    }

    this.frameBuffer.push(objData);
    this.updateSourceBuffer();
  }

  setVideoElement(video: HTMLVideoElement): void {
    this.videoElement = video;
  }

  setPlayMode(playback: boolean): void {
    this.isPlayback = playback;
  }

  clearBuffer(): void {
    // Clear frame buffer to stop processing new frames
    this.frameBuffer = [];
    // Clear source buffer if it exists and is not updating
    if (
      this.sourceBuffer
      && !this.sourceBuffer.updating
      && this.mediaSource
      && this.mediaSource.readyState === 'open'
    ) {
      try {
        const { buffered } = this.sourceBuffer;
        if (buffered.length > 0) {
          const end = buffered.end(buffered.length - 1);
          this.sourceBuffer.remove(0, end);
        }
      } catch {
        // Ignore errors during buffer clearing
      }
    }
    this.currentSegmentIndex = 0;
  }

  uninitMse(): void {
    this.stopCleanupTimer();

    if (this.videoElement !== null) {
      if (this.boundVideoErrorCallback) {
        this.videoElement.removeEventListener(
          'error',
          this.boundVideoErrorCallback
        );
      }
      try {
        if (
          this.videoElement.src
          && this.videoElement.src !== window.location.href
        ) {
          window.URL.revokeObjectURL(this.videoElement.src);
        }
      } catch {
        // ignore
      }
      this.videoElement.playbackRate = 1.0;
      this.videoElement.pause();
      this.videoElement.removeAttribute('src');
    }

    this.uninitSourceBuffer();
    if (this.mediaSource) {
      try {
        if (this.mediaSource.readyState === 'open') {
          this.mediaSource.endOfStream();
        }
      } catch {
        // Already closed
      }
    }
    this.mediaSource = null;
    this.videoElement = null;
    this.sourceBuffer = null;
    this.frameBuffer = [];
    this.updateend = 1;
    this.mimeCodec = '';
    this.initFlag = MsMediaSource.statusIdel;
  }

  /**
   * Clean up MSE state for stream switch while preserving the videoElement reference.
   * Unlike uninitMse(), this allows the same MsMediaSource instance to be reused
   * with a new stream (e.g., reload or stream switch without destroying the player).
   */
  cleanupForNewStream(): void {
    this.stopCleanupTimer();
    this.frameBuffer = [];
    this.lastSeekTime = 0;
    this.uninitSourceBuffer();
    if (this.mediaSource) {
      try {
        if (this.mediaSource.readyState === 'open') {
          this.mediaSource.endOfStream();
        }
      } catch {
        // Already closed
      }
    }
    if (
      this.videoElement?.src
      && this.videoElement.src !== window.location.href
    ) {
      try {
        window.URL.revokeObjectURL(this.videoElement.src);
      } catch {
        /* ignore */
      }
    }
    this.mediaSource = null;
    this.sourceBuffer = null;
    this.updateend = 1;
    this.mimeCodec = '';
    this.initFlag = MsMediaSource.statusIdel;
  }

  resetInitFlag(): void {
    this.initFlag = MsMediaSource.statusIdel;
  }

  getInitFlag(): number {
    return this.initFlag;
  }
}

export default MsMediaSource;
