/**
 * Shared player contracts. NAL units are parsed once in the adapter and indexed for downstream consumers.
 */

export type VideoCodec = 'h264' | 'h265';

export type TechKind = 'webcodecs' | 'mse' | 'wasm' | 'whep';

/** A NAL unit index; offset points to its normalized four-byte start code. */
export interface NaluIndex {
  /** Start-code offset in EncodedFrame.data. */
  offset: number;
  /** Number of bytes including the start code, up to the next NAL unit or frame end. */
  size: number;
  /** h264: header & 0x1f；h265: (header >> 1) & 0x3f */
  type: number;
}

/** Normalized adapter output: one Annex B access unit with indexed NAL units. */
export interface EncodedFrame {
  codec: VideoCodec;
  /** Normalized Annex B data with four-byte start codes; transferable. */
  data: Uint8Array;
  nalus: NaluIndex[];
  keyframe: boolean;
  /** Device timestamp in 90 kHz units, when available and validated. */
  pts90k?: number;
  dts90k?: number;
  /** Monotonically increasing sequence number within the adapter instance. */
  seq: number;
}

/** Static stream metadata parsed from SPS. */
export interface StreamMeta {
  codec: VideoCodec;
  width: number;
  height: number;
  fps?: number;
}

export type PlayerState =
  | 'idle'
  | 'connecting'
  | 'buffering'
  | 'playing'
  | 'paused'
  | 'degraded'
  | 'ended'
  | 'error';

export interface CTStats {
  state: PlayerState;
  tech: TechKind | null;
  codec: VideoCodec | null;
  /** Adapter output rate in frames per second, sampled once per second. */
  fps: number;
  /** Canvas draw calls per second during the previous sampling interval; unavailable for MSE. */
  renderedFps: number | null;
  /** Transport message rate per second. */
  pps: number;
  /** Estimated transport throughput in kB/s. */
  bitrateKBps: number;
  /** Bytes received. */
  bytes: number;
  /** MSE buffer end minus playback position in milliseconds, excluding end-to-end latency; null for other pipelines. */
  latencyMs: number | null;
  /** Span in milliseconds from the first MSE buffered range start to the last range end, including any gaps. */
  bufferedMs: number | null;
  /** Frames dropped due to buffering limits or before the first IDR. */
  droppedFrames: number;
  /** Decoded display size of the most recent rendered frame; MSE reports it via the video element instead. */
  videoWidth?: number;
  videoHeight?: number;
  /** Access units received by the current pipeline; reset when the pipeline is rebuilt. */
  pipelineFramesIn?: number;
  /** Canvas draw-call intervals; P95 covers the last five seconds, and stalls count completed intervals over 200 ms. */
  drawTiming?: { currentGapMs: number | null; p95GapMs: number | null; stalls: number };
  /** Decode errors reported by the current WASM pipeline; not equivalent to dropped frames. */
  decodeErrors?: number;
  /** Audio/video synchronization state for the timestamped WASM path. */
  avSync?: { status: 'waiting' | 'active' | 'unavailable'; driftMs: number | null; dropped: number; queued: number };
  /** WASM drop counts by reason; unavailable for other pipelines. */
  wasmDrops?: { beforeConfiguration: number; waitingForKeyframe: number; queueOverflow: number; suspended: number; presentation?: number };
  /** Timestamped WASM presentation queue state for diagnosing stalls. */
  wasmPresentation?: { queued: number; nextInMs: number | null; resets: number };
  /** WASM timings are cumulative per-session averages in milliseconds. */
  wasmPerf?: { decodeMs: number | null; roundTripMs: number | null; renderMs: number | null; queue: number; peakQueue: number; samples: number; initMs: number | null; recentDecodeMs?: number | null; recentDecodeP95Ms?: number | null; threads?: number | null };
  /** Number of technology fallback transitions. */
  fallbacks: number;
  /** Number of reconnects. */
  reconnects: number;
}

// ---------------------------------------------------------------------------
// SourceAdapter converts stream protocols into EncodedFrame objects.
// Runs in the transport worker or main-thread fallback; each push receives one WebSocket message.
// ---------------------------------------------------------------------------

export interface SourceAdapter {
  readonly name: string;
  /** Optional detection state used to bound startup waiting. */
  isDetecting?(): boolean;
  /** Process a binary transport message and return complete frames with transferable data. */
  push(chunk: Uint8Array, timestamp?: { pts90k?: number; dts90k?: number }): EncodedFrame[];
  /** Reset state after reconnecting and wait for parameter sets and an IDR. */
  reset(): void;
}

export type AdapterRef =
  | string
  | { name: string; options?: Record<string, unknown> }
  | (() => Promise<{ default: SourceAdapter } | SourceAdapter>);

export interface AdapterDescriptor {
  name: string;
  options?: Record<string, unknown>;
}

// ---------------------------------------------------------------------------
// Transport
// ---------------------------------------------------------------------------

export type TransportStatus =
  | { status: 'connecting' }
  | { status: 'open' }
  | { status: 'reconnecting'; attempt: number; delayMs: number }
  | { status: 'closed'; code: number; reason: string }
  | { status: 'superseded'; code: number; reason: string }
  | { status: 'retry-exhausted' }
  | { status: 'error'; message: string }
  | { status: 'parse-error'; message: string };

export interface ReconnectPolicy {
  baseDelayMs: number;
  maxDelayMs: number;
  jitterMs: number;
  maxAttempts: number;
  /** Retry window duration; the count resets after the window expires. */
  windowMs: number;
  supersededCodes: number[];
  supersededReasons: string[];
  /**
   * Whether to reconnect after a normal close (codes 1000–1005); defaults to false.
   * Enable this for devices that routinely close short sessions and expect clients to reconnect.
   */
  reconnectOnNormalClose: boolean;
}

// ---------------------------------------------------------------------------
// Worker protocol (transport-ws)
// ---------------------------------------------------------------------------

export type WorkerCommand =
  | {
      cmd: 'connect';
      url: string;
      adapter: AdapterDescriptor;
      reconnect?: Partial<ReconnectPolicy>;
    }
  | { cmd: 'disconnect' };

export type WorkerEvent =
  | { ev: 'status'; status: TransportStatus }
  | { ev: 'frame'; frame: EncodedFrame }
  | { ev: 'stats'; bytes: number; messages: number };

// ---------------------------------------------------------------------------
// Public configuration
// ---------------------------------------------------------------------------

export type LatencyPresetName = 'raw' | 'ultra' | 'balanced' | 'stable';

export type PlayMode = 'live' | 'playback';

export interface CTSource {
  transport: {
    type: 'ws';
    url: string;
    /** Optional reconnect policy overrides. */
    reconnect?: Partial<ReconnectPolicy>;
  };
  /** Stream adapter; defaults to automatic detection. */
  adapter?: AdapterRef;
  /** 'auto' detects the codec from the first frame (default). */
  codec?: VideoCodec | 'auto';
  /**
   * live (default): low-latency live-edge tracking;
   * playback: full buffering without live-edge tracking, with seek and speed controls (MSE only).
   */
  mode?: PlayMode;
  /** Optional raw PCM S16LE audio channel; configuring it enables audio. */
  audio?: {
    /** Explicit switch; defaults to true. Set to false to disable audio without removing its configuration. */
    enabled?: boolean;
    url: string;
    sampleRate?: number;
    channels?: number;
    /** Parse a complete PCM message and return its first sample's 90 kHz PTS on the video's clock. */
    parsePacket?: (message: Uint8Array) => { pcm: Uint8Array; pts90k: number } | null;
    /** Timestamped WASM sync is enabled when parsePacket is provided; false disables sync. */
    sync?: { maxDriftMs?: number } | false;
  };
}

export interface FallbackPolicy {
  /** Technology fallback chain in priority order; generated from detected capabilities by default. */
  techChain?: TechKind[];
  /** Whether to request a stream switch when hardware decoding is unavailable; defaults to true. */
  allowStreamFallback: boolean;
  /** Whether to fall back to WASM software decoding; defaults to true. An explicit techChain is handled separately. */
  allowQualityFallback: boolean;
  /** Maximum watchdog restarts on one technology before moving to the next fallback. */
  maxInternalRestarts: number;
}

export interface CTPlayerOptions {
  latency?: LatencyPresetName;
  objectFit?: 'contain' | 'cover';
  fallback?: Partial<FallbackPolicy>;
  /** WASM kernel configuration. Values above one thread require cross-origin isolation. */
  wasm?: { kernelBaseUrl?: string; threads?: number };
  /** Log level; defaults to 'warn'. */
  logLevel?: 'debug' | 'info' | 'warn' | 'silent';
}

/** Stream fallback request: the host switches the backend codec and reports the result. */
export interface StreamChangeRequest {
  codec: VideoCodec;
  reason: 'codec-mismatch' | 'decode-fallback';
  current: { codec: VideoCodec; tech: TechKind | null };
}

export interface StreamChangeResponse {
  accepted: boolean;
  /** Optional replacement stream URL; the player reconnects to it when supplied. */
  url?: string;
}
