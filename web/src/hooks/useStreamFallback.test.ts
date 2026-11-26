import { act, renderHook } from '@testing-library/react';
import { describe, expect, it } from 'vitest';
import { useStreamFallback } from './useStreamFallback';
import type { MediaStreamStatus } from './useMediaStatus';

const stream = (id: string, codec: string, enabled = true): MediaStreamStatus => ({
  stream_id: id,
  codec,
  has_encoder: enabled,
  status: 'active',
  width: 1920,
  height: 1080,
  fps: 30,
  bitrate_bps: 1000000,
  gop: 30,
});

describe('stream codec fallback', () => {
  it('tries the selected H.265 stream before falling back', () => {
    const streams = [stream('sub', 'h265'), stream('main', 'h264')];
    const { result } = renderHook(() => useStreamFallback({ streams, activeStream: 'sub' }));
    expect(result.current.effectiveStreamId).toBe('sub');
    expect(result.current.codecNotice).toBeNull();
    act(() => window.dispatchEvent(new CustomEvent('wv_error', { detail: { type: 'codec_mismatch' } })));
    expect(result.current.effectiveStreamId).toBe('main');
    expect(result.current.codecNotice).toBe('hevc_unsupported_fallback');
  });

  it('does not choose a disabled H.264 stream after a codec failure', () => {
    const streams = [stream('sub', 'h265'), stream('main', 'h264', false)];
    const { result } = renderHook(() => useStreamFallback({ streams, activeStream: 'sub' }));
    act(() => window.dispatchEvent(new CustomEvent('wv_error', { detail: { type: 'codec_mismatch' } })));
    expect(result.current.effectiveStreamId).toBe('sub');
    expect(result.current.codecNotice).toBe('hevc_unsupported_no_h264');
  });
});
