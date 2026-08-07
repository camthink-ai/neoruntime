import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
  clearCriticalDeviceTransition,
  enterCriticalDeviceTransition,
  isCriticalDeviceTransitionActive,
} from './deviceTransition';

describe('deviceTransition', () => {
  beforeEach(() => {
    vi.useFakeTimers();
    vi.setSystemTime(new Date('2026-07-29T00:00:00Z'));
    clearCriticalDeviceTransition();
    window.localStorage.clear();
  });

  afterEach(() => {
    clearCriticalDeviceTransition();
    window.localStorage.clear();
    vi.useRealTimers();
  });

  it('stays active until the release function is called', () => {
    const release = enterCriticalDeviceTransition(60_000);

    expect(isCriticalDeviceTransitionActive()).toBe(true);

    release();

    expect(isCriticalDeviceTransitionActive()).toBe(false);
  });

  it('keeps the transition active from persisted ttl after a reload', () => {
    window.localStorage.setItem(
      'aipc.deviceTransition.until',
      String(Date.now() + 60_000)
    );

    expect(isCriticalDeviceTransitionActive()).toBe(true);
  });

  it('expires the persisted transition window', () => {
    window.localStorage.setItem(
      'aipc.deviceTransition.until',
      String(Date.now() - 1)
    );

    expect(isCriticalDeviceTransitionActive()).toBe(false);
    expect(window.localStorage.getItem('aipc.deviceTransition.until')).toBeNull();
  });
});
