/**
 * Session-cached browser capability detection. Stream-specific SPS support is
 * checked by the pipeline after receiving parameter sets.
 */

export interface PlatformInfo {
  iosWebkit: boolean;
  safari: boolean;
  firefox: boolean;
  android: boolean;
  mobile: boolean;
}

export interface CapabilityProfile {
  webcodecs: boolean;
  webcodecsH264: boolean;
  /** HEVC WebCodecs path may be attempted; actual SPS configuration is checked during playback. */
  webcodecsH265: boolean;
  mse: boolean;
  mseH264: boolean;
  mseH265: boolean;
  wasm: boolean;
  wasmSimd: boolean;
  wasmThreads: boolean;
  platform: PlatformInfo;
}

/**
 * MSE HEVC 探测候选串。必须与 hevc-muxer 的 pickHevcMimeCodec 生成的格式一致
 * （8 位 hex 兼容标志位）。畸形串（如 hvc1.1.6.L93）会被 isTypeSupported 拒绝，
 * 造成误判「不支持」。
 */
const HEVC_MSE_CANDIDATES = [
  'video/mp4; codecs="hvc1.1.60000000.L93"',
  'video/mp4; codecs="hvc1.1.60000000.L120"',
  'video/mp4; codecs="hvc1.1.60000000.L150"',
  'video/mp4; codecs="hev1.1.60000000.L120"',
  'video/mp4; codecs="hvc1.2.60000000.L120"',
  'video/mp4; codecs="hvc1.2.40000000.L120"',
  'video/mp4; codecs="hvc1.0.1.60000000.L120.B0"',
];

export function detectPlatform(): PlatformInfo {
  if (typeof navigator === 'undefined') {
    return { iosWebkit: false, safari: false, firefox: false, android: false, mobile: false };
  }
  const ua = navigator.userAgent;
  const isIPadOSDisguised =
    /Macintosh/.test(ua) && typeof navigator.maxTouchPoints === 'number' && navigator.maxTouchPoints > 1;
  const iosWebkit =
    (/iPhone|iPad|iPod/.test(ua) || isIPadOSDisguised) &&
    /AppleWebKit/.test(ua) &&
    !/Chrome|CriOS|Edg|FxiOS/.test(ua);
  return {
    iosWebkit,
    safari: /Safari/.test(ua) && !/Chrome|CriOS|Edg|FxiOS|OPR/.test(ua),
    firefox: /Firefox|FxiOS/.test(ua),
    android: /Android/.test(ua),
    mobile: /Mobi|Android|iPhone|iPad|iPod/.test(ua) || isIPadOSDisguised,
  };
}

function getMediaSourceCtor(): (typeof MediaSource) | null {
  if (typeof window === 'undefined') return null;
  const w = window as unknown as {
    ManagedMediaSource?: typeof MediaSource;
    MediaSource?: typeof MediaSource;
  };
  return w.ManagedMediaSource ?? w.MediaSource ?? null;
}

function isTypeSupportedSafe(ms: typeof MediaSource, mime: string): boolean {
  try {
    return ms.isTypeSupported(mime);
  } catch {
    return false;
  }
}

async function probeWebCodecs(codec: string): Promise<boolean> {
  try {
    if (typeof VideoDecoder === 'undefined') return false;
    const support = await VideoDecoder.isConfigSupported({
      codec,
      optimizeForLatency: true,
    } as VideoDecoderConfig);
    return support.supported === true;
  } catch {
    return false;
  }
}

let cached: Promise<CapabilityProfile> | null = null;

export function detectCapabilities(): Promise<CapabilityProfile> {
  if (cached) return cached;
  cached = (async (): Promise<CapabilityProfile> => {
    const platform = detectPlatform();
    const ms = getMediaSourceCtor();

    const webcodecs =
      typeof VideoDecoder !== 'undefined' && typeof EncodedVideoChunk !== 'undefined';
    const [webcodecsH264] = await Promise.all([
      webcodecs ? probeWebCodecs('avc1.42E01E') : Promise.resolve(false),
    ]);
    // The actual HEVC profile and level are unknown until SPS arrives. Keep this
    // path eligible; WebCodecsPipeline checks the exact stream configuration.
    const webcodecsH265 = webcodecs;

    const mse = ms !== null;
    const mseH264 = ms !== null && isTypeSupportedSafe(ms, 'video/mp4; codecs="avc1.42E01E"');
    const mseH265 =
      ms !== null && HEVC_MSE_CANDIDATES.some((c) => isTypeSupportedSafe(ms, c));

    const wasm = typeof WebAssembly === 'object';
    let wasmSimd = false;
    if (wasm) {
      try {
        // (module (func (result v128) (v128.const i32x4 0 0 0 0))) 的二进制魔数探测
        wasmSimd = WebAssembly.validate(
          new Uint8Array([
            0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x01, 0x05, 0x01, 0x60,
            0x00, 0x01, 0x7b, 0x03, 0x02, 0x01, 0x00, 0x0a, 0x0b, 0x01, 0x09, 0x00,
            0xfd, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x0b,
          ])
        );
      } catch {
        wasmSimd = false;
      }
    }

    return {
      webcodecs,
      webcodecsH264,
      webcodecsH265,
      mse,
      mseH264,
      mseH265,
      wasm,
      wasmSimd,
      wasmThreads: wasm && typeof SharedArrayBuffer !== 'undefined',
      platform,
    };
  })();
  return cached;
}

/** Clear the capability cache for testing. */
export function resetCapabilityCache(): void {
  cached = null;
}
