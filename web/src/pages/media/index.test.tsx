import { fireEvent, render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import Monitoring from './index';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({ t: (_key: string, fallback: string) => fallback }),
}));

vi.mock('@/utils/storage', () => ({
  getItem: () => 'test-token',
}));

const state = vi.hoisted(() => ({ codec: 'h264', hevcSupported: true }));

vi.mock('@/lib/videoStream/hevcSupport', () => ({
  isHevcSupported: () => state.hevcSupported,
}));

vi.mock('./hooks/useMedia', () => ({
  useMediaStatus: () => ({
    data: {
      streams: [{
        stream_id: 'main',
        status: 'active',
        has_encoder: true,
        codec: state.codec,
        width: 1920,
        height: 1080,
      }],
    },
  }),
}));

vi.mock('@/components/player/player', () => ({
  default: ({
    videoUrl,
    streamRestarting,
  }: {
    videoUrl: string;
    streamRestarting: boolean;
  }) => (
    <div
      data-testid="player"
      data-video-url={videoUrl}
      data-stream-restarting={String(streamRestarting)}
    />
  ),
}));

vi.mock('./components/MediaSettings', () => ({
  default: ({
    onEncoderRestartingChange,
    onStreamToggleSuccess,
  }: {
    onEncoderRestartingChange: (active: boolean) => void;
    onStreamToggleSuccess: (streamId: string, enabled: boolean) => void;
  }) => (
    <>
      <button type="button" onClick={() => onEncoderRestartingChange(true)}>
        start reconfigure
      </button>
      <button type="button" onClick={() => onEncoderRestartingChange(false)}>
        finish reconfigure
      </button>
      <button type="button" onClick={() => onStreamToggleSuccess('sub', true)}>
        finish unrelated stream toggle
      </button>
    </>
  ),
}));

describe('Monitoring encoder reconfiguration', () => {
  beforeEach(() => {
    window.localStorage.clear();
    state.codec = 'h264';
    state.hevcSupported = true;
  });

  it('shows an unsupported preview for H.265 without changing streams', () => {
    state.codec = 'h265';
    state.hevcSupported = false;
    render(<Monitoring />);
    expect(screen.getByRole('status').textContent).toContain('不支持 H.265');
    expect(screen.queryByTestId('player')).toBeNull();
  });

  it('plays H.265 when supported and handles a runtime codec mismatch', () => {
    state.codec = 'h265';
    render(<Monitoring />);
    expect(screen.getByTestId('player').getAttribute('data-video-url')).toContain('/main?');
    fireEvent(window, new CustomEvent('wv_error', { detail: { type: 'codec_mismatch' } }));
    expect(screen.getByRole('status').textContent).toContain('不支持 H.265');
    expect(screen.queryByTestId('player')).toBeNull();
  });

  it('disconnects the old player until the final stream is ready', () => {
    render(<Monitoring />);

    const player = screen.getByTestId('player');
    expect(player.getAttribute('data-video-url')).toContain(
      '/api/v1/h264/main?token=test-token'
    );

    fireEvent.click(screen.getByRole('button', { name: 'start reconfigure' }));
    expect(player.getAttribute('data-video-url')).toBe('');
    expect(player.getAttribute('data-stream-restarting')).toBe('true');

    fireEvent.click(screen.getByRole('button', { name: 'finish reconfigure' }));
    expect(player.getAttribute('data-video-url')).toContain(
      '/api/v1/h264/main?token=test-token'
    );
    expect(player.getAttribute('data-stream-restarting')).toBe('false');
  });

  it('refreshes the active player after another stream rebuilds the pipeline', () => {
    render(<Monitoring />);

    const player = screen.getByTestId('player');
    const previousUrl = player.getAttribute('data-video-url');

    fireEvent.click(
      screen.getByRole('button', { name: 'finish unrelated stream toggle' })
    );

    expect(player.getAttribute('data-video-url')).not.toBe(previousUrl);
    expect(player.getAttribute('data-video-url')).toContain('reconnect=1');
  });
});
