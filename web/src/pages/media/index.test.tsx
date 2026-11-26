import { fireEvent, render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import Monitoring from './index';
import type { MediaStreamStatus } from '@/hooks/useMediaStatus';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({ t: (_key: string, fallback?: string) => fallback ?? _key }),
}));

vi.mock('@/utils/storage', () => ({
  getItem: () => 'test-token',
}));

const state = vi.hoisted(() => ({
  streams: [] as MediaStreamStatus[],
}));

vi.mock('./hooks/useMedia', () => ({
  useMediaStatus: () => ({ data: { streams: state.streams } }),
}));

vi.mock('@/components/player/player', () => ({
  default: ({
    videoUrl,
    configuring,
  }: {
    videoUrl: string;
    configuring: boolean;
  }) => (
    <div
      data-testid="player"
      data-video-url={videoUrl}
      data-configuring={String(configuring)}
    />
  ),
}));

vi.mock('./components/MediaSettings', () => ({
  default: ({
    onStreamToggleSuccess,
    onEncoderConfiguringChange,
  }: {
    onStreamToggleSuccess: (streamId: string) => void;
    onEncoderConfiguringChange: (active: boolean) => void;
  }) => (
    <>
      <button type="button" onClick={() => onEncoderConfiguringChange(true)}>
        start reconfigure
      </button>
      <button type="button" onClick={() => onEncoderConfiguringChange(false)}>
        finish reconfigure
      </button>
      <button type="button" onClick={() => onStreamToggleSuccess('main')}>
        toggle active stream
      </button>
    </>
  ),
}));

const stream = (id: string, codec: string): MediaStreamStatus => ({
  stream_id: id,
  status: 'active',
  has_encoder: true,
  codec,
  width: 1920,
  height: 1080,
  fps: 30,
  bitrate_bps: 1000000,
  gop: 30,
});

describe('Monitoring (CT player integration)', () => {
  beforeEach(() => {
    window.localStorage.clear();
    state.streams = [stream('main', 'h264')];
  });

  it('starts H.265 playback directly instead of pre-switching to H.264', () => {
    state.streams = [stream('main', 'h265'), stream('sub', 'h264')];
    render(<Monitoring />);
    expect(screen.getByTestId('player').getAttribute('data-video-url'))
      .toContain('/api/v1/h264/main?');
    // No warning banner until the engine reports software decoding.
    expect(screen.queryByRole('status')).toBeNull();
  });

  it('falls back to an H.264 stream after a runtime codec mismatch', () => {
    state.streams = [stream('main', 'h265'), stream('sub', 'h264')];
    render(<Monitoring />);
    fireEvent(
      window,
      new CustomEvent('wv_error', { detail: { type: 'codec_mismatch' } })
    );
    expect(screen.getByTestId('player').getAttribute('data-video-url'))
      .toContain('/api/v1/h264/sub?');
  });

  it('tracks encoder configuration state on the player', () => {
    render(<Monitoring />);
    const player = screen.getByTestId('player');
    expect(player.getAttribute('data-video-url')).toContain(
      '/api/v1/h264/main?token=test-token'
    );
    expect(player.getAttribute('data-configuring')).toBe('false');

    fireEvent.click(screen.getByRole('button', { name: 'start reconfigure' }));
    expect(player.getAttribute('data-configuring')).toBe('true');

    fireEvent.click(screen.getByRole('button', { name: 'finish reconfigure' }));
    expect(player.getAttribute('data-configuring')).toBe('false');
  });

  it('remounts the player after the active stream is toggled', () => {
    render(<Monitoring />);
    expect(screen.getByTestId('player').getAttribute('data-video-url'))
      .toContain('reconnect=0');

    fireEvent.click(
      screen.getByRole('button', { name: 'toggle active stream' })
    );
    // The key change remounts the mocked player — re-query for the new node.
    expect(screen.getByTestId('player').getAttribute('data-video-url'))
      .toContain('reconnect=1');
  });
});
