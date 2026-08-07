import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { useOsdConfig, MAX_IMAGE_OVERLAYS } from './useOsdConfig';

const mocks = vi.hoisted(() => ({
  fetchOsdConfig: vi.fn(),
  updateOsdConfig: vi.fn(),
  toast: { error: vi.fn() },
}));

vi.mock('react-i18next', () => {
  // Stable `t` — react-i18next memoizes it in production.
  const t = (_key: string, fallback?: string) => fallback ?? _key;
  return { useTranslation: () => ({ t }) };
});

vi.mock('sonner', () => ({ toast: mocks.toast }));

vi.mock('@/services/media', () => ({
  fetchOsdConfig: mocks.fetchOsdConfig,
  updateOsdConfig: mocks.updateOsdConfig,
}));

describe('useOsdConfig', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.fetchOsdConfig.mockResolvedValue({
      streams: [{ stream_name: 'main', image_overlays: [] }],
    });
    mocks.updateOsdConfig.mockResolvedValue(undefined);
  });

  it('caps image overlays at MAX_IMAGE_OVERLAYS per stream', async () => {
    const { result } = renderHook(
      ({ stream }) => useOsdConfig(stream, true),
      { initialProps: { stream: 'main' as const } }
    );

    await waitFor(() => expect(result.current.loading).toBe(false));

    // Add up to the cap — each add appends one overlay slot.
    for (let i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
      act(() => result.current.addImageOverlay());
    }
    await waitFor(() => expect(result.current.imageOverlays).toHaveLength(MAX_IMAGE_OVERLAYS));

    // One more add must be rejected — the cap holds even past the UI disable.
    act(() => result.current.addImageOverlay());
    await waitFor(() => expect(result.current.imageOverlays).toHaveLength(MAX_IMAGE_OVERLAYS));
  });
});
