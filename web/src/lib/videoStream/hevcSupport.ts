/**
 * Browser HEVC (H.265) playback capability probe.
 *
 * The codec strings here MUST match the format produced by `hevcMp4.ts` ->
 * `pickMimeCodec`, which builds them from the real SPS PTL:
 *   `hvc1.<profileIdc>.<compatHex>.<tier><levelIdc>`   (candidate #2)
 *   `hvc1.<profileSpace>.<profileIdc>.<compatHex>.<tier><levelIdc>.B0` (candidate #1)
 * Use the 8-hex-digit compatibility flags (e.g. `60000000` for Main profile) —
 * malformed strings like `hvc1.1.6.L93.B0` are rejected by isTypeSupported
 * and cause a false "unsupported" verdict.
 *
 * We probe Main (profileIdc=1, compat=0x60000000) and Main10 (profileIdc=2)
 * at a few common levels; if any is supported the stream is playable.
 */

let cached: boolean | null = null;

const HEVC_CODEC_CANDIDATES = [
  // Main profile — short form (no profileSpace, no constraint), matches pickMimeCodec candidate #2
  'video/mp4; codecs="hvc1.1.60000000.L93"',
  'video/mp4; codecs="hvc1.1.60000000.L120"',
  'video/mp4; codecs="hvc1.1.60000000.L150"',
  'video/mp4; codecs="hev1.1.60000000.L120"',
  // Main10 profile
  'video/mp4; codecs="hvc1.2.60000000.L120"',
  'video/mp4; codecs="hvc1.2.40000000.L120"',
  'video/mp4; codecs="hev1.2.60000000.L120"',
  // Long form (profileSpace + constraint), matches pickMimeCodec candidate #1
  'video/mp4; codecs="hvc1.0.1.60000000.L120.B0"',
  'video/mp4; codecs="hev1.0.1.60000000.L120.B0"',
];

export function isHevcSupported(): boolean {
  if (cached !== null) return cached;
  if (typeof window === 'undefined') {
    cached = false;
    return false;
  }
  const MS =    (window as unknown as { ManagedMediaSource?: typeof MediaSource })
      .ManagedMediaSource ?? window.MediaSource;
  if (!MS?.isTypeSupported) {
    cached = false;
    return false;
  }
  cached = HEVC_CODEC_CANDIDATES.some(c => {
    try {
      return MS.isTypeSupported(c);
    } catch {
      return false;
    }
  });
  return cached;
}
