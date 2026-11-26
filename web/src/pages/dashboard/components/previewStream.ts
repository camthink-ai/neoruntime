import type { MediaStreamStatus } from '@/hooks/useMediaStatus';

/** Choose the smallest configured, enabled stream for the dashboard preview. */
export function selectPreviewStream(streams: MediaStreamStatus[]): string {
  const smallest = streams.reduce<MediaStreamStatus | null>((best, stream) => {
    if (!stream.has_encoder || !Number.isFinite(stream.width) || !Number.isFinite(stream.height) || stream.width <= 0 || stream.height <= 0) return best;
    if (!best || stream.width * stream.height < best.width * best.height) return stream;
    return best;
  }, null);
  return smallest?.stream_id ?? streams.find(stream => stream.has_encoder)?.stream_id ?? 'sub';
}
