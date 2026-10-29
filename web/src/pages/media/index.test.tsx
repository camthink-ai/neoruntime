import { fireEvent, render, screen } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import Monitoring from './index';

vi.mock('react-i18next', () => ({
  useTranslation: () => ({ t: (_key: string, fallback: string) => fallback }),
}));

vi.mock('@/utils/storage', () => ({
  getItem: () => 'test-token',
}));

vi.mock('./hooks/useMedia', () => ({
  useMediaStatus: () => ({
    data: {
      streams: [{
        stream_id: 'main',
        status: 'active',
        has_encoder: true,
        codec: 'h264',
        width: 1920,
        height: 1080,
      }],
    },
  }),
}));

vi.mock('@/hooks/useStreamFallback', () => ({
  useStreamFallback: ({ streams }: { streams: any[] }) => ({
    effectiveStreamId: 'main',
    effectiveStreamInfo: streams[0],
    codecNotice: null,
  }),
  useHevcNoticeDismissal: (): [boolean, () => void] => [false, vi.fn()],
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
