import { useEffect, useMemo, useState } from 'react';
import { isHevcSupported } from '@/lib/videoStream/hevcSupport';
import type { MediaStreamStatus } from '@/hooks/useMediaStatus';

/** Pick the best H.264 stream to fall back to when HEVC can't be decoded. */
export function findH264Fallback(
  streams: MediaStreamStatus[],
  activeStream: string
): MediaStreamStatus | null {
  const isH264 = (s: MediaStreamStatus) => (s.codec ?? 'h264').toLowerCase() === 'h264';
  const STREAM_ORDER = ['main', 'sub', 'third'];
  return (
    streams.find(s => s.stream_id === activeStream && isH264(s))
    || streams.find(s => isH264(s))
    || STREAM_ORDER
      .map(id => streams.find(s => s.stream_id === id))
      .find((s): s is MediaStreamStatus => !!s && isH264(s))
    || null
  );
}

export type CodecNotice =
  | 'hevc_unsupported_fallback'
  | 'hevc_unsupported_no_h264'
  | null;

export interface UseStreamFallbackArgs {
  streams: MediaStreamStatus[];
  activeStream: string;
}

export interface UseStreamFallbackResult {
  effectiveStreamId: string;
  effectiveStreamInfo: MediaStreamStatus | undefined;
  codecNotice: CodecNotice;
}

/**
 * Shared codec-fallback logic for the live `Player` on the media page and the
 * devices Overlay tab.
 *
 * - Probes HEVC MSE support once (`isHevcSupported`); when the browser can't
 *   decode H.265 we proactively play an H.264 stream instead of letting the
 *   player fail with codecMismatch.
 * - Reactive safety net: if the real codec probe (pickMimeCodec, using the
 *   actual SPS) fails after connect (window `wv_error` with
 *   `detail.type === 'codec_mismatch'`), fall back to an H.264 stream.
 * - Resets the reactive fallback whenever the user switches streams.
 *
 * Behavior-preserving extract from `pages/media/index.tsx`.
 */
export function useStreamFallback({
  streams,
  activeStream,
}: UseStreamFallbackArgs): UseStreamFallbackResult {
  // Reactive fallback set when the player fires codec_mismatch at runtime
  // (real SPS-derived codec unsupported even though the pre-probe said OK).
  const [reactiveFallbackId, setReactiveFallbackId] = useState<string | null>(
    null
  );
  const [reactiveNoH264, setReactiveNoH264] = useState(false);

  // Probe HEVC MSE support once.
  const hevcSupported = useMemo(() => isHevcSupported(), []);

  // activeStream stays as the user's selection; effectiveStreamId is what we
  // actually feed to the player.
  const { effectiveStreamId, effectiveStreamInfo, codecNotice } = useMemo(() => {
    const selected = streams.find(s => s.stream_id === activeStream);
    const selectedCodec = (selected?.codec ?? 'h264').toLowerCase();

    // Reactive override (codec_mismatch fired at runtime) takes precedence.
    if (reactiveFallbackId) {
      const fb = streams.find(s => s.stream_id === reactiveFallbackId);
      return {
        effectiveStreamId: reactiveFallbackId,
        effectiveStreamInfo: fb,
        codecNotice: 'hevc_unsupported_fallback' as CodecNotice,
      };
    }
    if (reactiveNoH264) {
      return {
        effectiveStreamId: activeStream,
        effectiveStreamInfo: selected,
        codecNotice: 'hevc_unsupported_no_h264' as CodecNotice,
      };
    }

    if (hevcSupported || selectedCodec !== 'h265') {
      return {
        effectiveStreamId: activeStream,
        effectiveStreamInfo: selected,
        codecNotice: null,
      };
    }

    // Selected stream is H.265 but browser can't decode it — fall back to H.264.
    const fallback = findH264Fallback(streams, activeStream);
    if (fallback) {
      return {
        effectiveStreamId: fallback.stream_id,
        effectiveStreamInfo: fallback,
        codecNotice: 'hevc_unsupported_fallback' as CodecNotice,
      };
    }

    return {
      effectiveStreamId: activeStream,
      effectiveStreamInfo: selected,
      codecNotice: 'hevc_unsupported_no_h264' as CodecNotice,
    };
  }, [activeStream, streams, hevcSupported, reactiveFallbackId, reactiveNoH264]);

  // Reset reactive fallback when the user switches streams.
  useEffect(() => {
    setReactiveFallbackId(null);
    setReactiveNoH264(false);
  }, [activeStream]);

  // Reactive safety net: if the real codec probe (pickMimeCodec, using the
  // actual SPS) fails after connect, fall back to an H.264 stream.
  useEffect(() => {
    const onWvError = (e: Event) => {
      const { detail } = e as CustomEvent<{ type?: string }>;
      if (detail?.type !== 'codec_mismatch') return;
      const fb = findH264Fallback(streams, activeStream);
      if (fb) {
        setReactiveFallbackId(fb.stream_id);
      } else {
        setReactiveNoH264(true);
      }
    };
    window.addEventListener('wv_error', onWvError);
    return () => window.removeEventListener('wv_error', onWvError);
  }, [streams, activeStream]);

  return { effectiveStreamId, effectiveStreamInfo, codecNotice };
}
