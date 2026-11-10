import { renderHook } from '@testing-library/react';
import { describe, expect, it, vi } from 'vitest';
import { useStreamFallback } from './useStreamFallback';
import type { MediaStreamStatus } from './useMediaStatus';

const capability = vi.hoisted(() => ({ supported: false }));
vi.mock('@/lib/videoStream/hevcSupport', () => ({
  isHevcSupported: () => capability.supported,
}));

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
  it('plays an enabled H.264 stream when H.265 is unsupported', () => {
    capability.supported = false;
    const streams = [stream('sub', 'h265'), stream('main', 'h264'), stream('third', 'h264', false)];
    const { result } = renderHook(() => useStreamFallback({ streams, activeStream: 'sub' }));
    expect(result.current.effectiveStreamId).toBe('main');
  });

  it('keeps the H.265 stream when supported', () => {
    capability.supported = true;
    const streams = [stream('sub', 'h265'), stream('main', 'h264')];
    const { result } = renderHook(() => useStreamFallback({ streams, activeStream: 'sub' }));
    expect(result.current.effectiveStreamId).toBe('sub');
  });

  it('does not choose a disabled H.264 stream', () => {
    capability.supported = false;
    const streams = [stream('sub', 'h265'), stream('main', 'h264', false)];
    const { result } = renderHook(() => useStreamFallback({ streams, activeStream: 'sub' }));
    expect(result.current.codecNotice).toBe('hevc_unsupported_no_h264');
  });
});
