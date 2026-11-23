export type CTErrorCode =
  | 'NETWORK_ERROR'
  | 'AUTH_FAILED'
  | 'SUPERSEDED'
  | 'RETRY_EXHAUSTED'
  | 'PROTOCOL_PARSE_ERROR'
  | 'STREAM_FORMAT_ERROR'
  | 'NO_KEYFRAME_TIMEOUT'
  | 'CODEC_UNSUPPORTED'
  | 'CODEC_MISMATCH'
  | 'DECODE_ERROR'
  | 'CONFIGURE_FAILED'
  | 'WORKER_CRASHED'
  | 'WASM_LOAD_FAILED'
  | 'RENDER_INIT_FAILED'
  | 'DESTROYED';

export interface CTError {
  code: CTErrorCode;
  /** fatal=true means playback stopped after exhausting recovery; false reports a handled or recoverable error. */
  fatal: boolean;
  tech?: 'webcodecs' | 'mse' | 'wasm' | 'whep';
  detail?: unknown;
  /** Human-readable recovery action taken by the engine. */
  recovery?: string;
}

export function ctError(
  code: CTErrorCode,
  opts: { fatal?: boolean; tech?: CTError['tech']; detail?: unknown; recovery?: string } = {}
): CTError {
  return {
    code,
    fatal: opts.fatal ?? true,
    tech: opts.tech,
    detail: opts.detail,
    recovery: opts.recovery,
  };
}
