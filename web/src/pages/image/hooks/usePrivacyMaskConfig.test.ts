import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { usePrivacyMaskConfig } from './usePrivacyMaskConfig';

const mocks = vi.hoisted(() => ({
  fetchPrivacyMaskConfig: vi.fn(),
  updatePrivacyMaskConfig: vi.fn(),
  toast: { error: vi.fn() },
}));

vi.mock('react-i18next', () => {
  // Stable `t` reference — react-i18next memoizes it in production, so the
  // mock must too, or effect deps like [enabled, t] re-fire every render.
  const t = (_key: string, fallback?: string) => fallback ?? _key;
  return { useTranslation: () => ({ t }) };
});

vi.mock('sonner', () => ({ toast: mocks.toast }));

vi.mock('@/services/media', () => ({
  fetchPrivacyMaskConfig: mocks.fetchPrivacyMaskConfig,
  updatePrivacyMaskConfig: mocks.updatePrivacyMaskConfig,
}));

const deviceConfig = {
  enabled: true,
  blur_radius: 0,
  color: 0,
  regions: [],
  dpm_enabled: false,
  dpm_labels: '',
  dpm_mode: 'mosaic',
  dpm_color: 0,
};

describe('usePrivacyMaskConfig', () => {
  beforeEach(() => {
    vi.clearAllMocks();
    mocks.fetchPrivacyMaskConfig.mockResolvedValue(deviceConfig);
    mocks.updatePrivacyMaskConfig.mockResolvedValue(undefined);
  });

  it('flushes the final debounced update when leaving the overlay tab', async () => {
    const { result, rerender } = renderHook(
      ({ enabled }) => usePrivacyMaskConfig(enabled),
      { initialProps: { enabled: true } }
    );

    await waitFor(() => expect(result.current.loading).toBe(false));

    act(() => {
      result.current.handleToggleEnabled(false);
    });
    rerender({ enabled: false });

    await waitFor(() => expect(mocks.updatePrivacyMaskConfig).toHaveBeenCalledWith({
        enabled: false,
      }));
  });

  it('does not revert an optimistic DPM toggle when a stale GET resolves mid-save', async () => {
    // Regression: a GET in flight before the toggle (tab re-entry) can resolve
    // WHILE the debounced PUT is still arming the device, returning the pre-arm
    // dpm_enabled=false. Blindly applying it reverted the optimistic ON → the
    // "打开先关闭再打开" flicker. Only the authoritative post-PUT re-sync may
    // correct arm-state; a stale non-authoritative GET must preserve the toggle.
    const { result, rerender } = renderHook(
      ({ enabled }) => usePrivacyMaskConfig(enabled),
      { initialProps: { enabled: true } }
    );

    await waitFor(() => expect(result.current.loading).toBe(false));
    expect(result.current.config!.dpm_enabled).toBe(false);

    // Hold the PUT pending so a stale GET can race against it.
    let putResolve!: () => void;
    mocks.updatePrivacyMaskConfig.mockReturnValueOnce(
      new Promise<void>(r => { putResolve = r; })
    );
    act(() => {
      result.current.handleDpmToggle(true);
    });
    // Optimistic ON is immediate.
    expect(result.current.config!.dpm_enabled).toBe(true);

    // Wait for the 400ms debounced save to fire (PUT now in flight).
    await waitFor(() => expect(mocks.updatePrivacyMaskConfig).toHaveBeenCalledWith({
      dpm_enabled: true,
    }));

    // Tab re-entry triggers a non-authoritative GET returning the STALE
    // pre-arm state. This must NOT revert the toggle to OFF.
    mocks.fetchPrivacyMaskConfig.mockResolvedValue({ ...deviceConfig, dpm_enabled: false });
    rerender({ enabled: false });
    rerender({ enabled: true });
    await waitFor(() => expect(result.current.config).not.toBeNull());
    expect(result.current.config!.dpm_enabled).toBe(true);

    // The authoritative post-PUT re-sync returns the post-arm truth (true).
    mocks.fetchPrivacyMaskConfig.mockResolvedValue({ ...deviceConfig, dpm_enabled: true });
    await act(async () => {
      putResolve();
    });
    await waitFor(() => expect(result.current.config!.dpm_enabled).toBe(true));
  });
});
