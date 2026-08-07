import { useSyncExternalStore } from 'react';

const TRANSITION_UNTIL_KEY = 'aipc.deviceTransition.until';
const DEFAULT_TRANSITION_TTL_MS = 2 * 60 * 60 * 1000;

let transitionDepth = 0;
const listeners = new Set<() => void>();

const canUseStorage = () => typeof window !== 'undefined' && !!window.localStorage;

const emit = () => {
  listeners.forEach(listener => listener());
};

const readPersistedUntil = () => {
  if (!canUseStorage()) return 0;
  const raw = window.localStorage.getItem(TRANSITION_UNTIL_KEY);
  const until = Number(raw);
  if (!Number.isFinite(until) || until <= Date.now()) {
    window.localStorage.removeItem(TRANSITION_UNTIL_KEY);
    return 0;
  }
  return until;
};

const persistUntil = (until: number) => {
  if (!canUseStorage()) return;
  window.localStorage.setItem(TRANSITION_UNTIL_KEY, String(until));
};

const clearPersisted = () => {
  if (!canUseStorage()) return;
  window.localStorage.removeItem(TRANSITION_UNTIL_KEY);
};

export function enterCriticalDeviceTransition(
  ttlMs = DEFAULT_TRANSITION_TTL_MS
) {
  transitionDepth += 1;
  persistUntil(Date.now() + ttlMs);
  emit();

  return () => {
    transitionDepth = Math.max(0, transitionDepth - 1);
    if (transitionDepth === 0) {
      clearPersisted();
    }
    emit();
  };
}

export function clearCriticalDeviceTransition() {
  transitionDepth = 0;
  clearPersisted();
  emit();
}

export function isCriticalDeviceTransitionActive() {
  return transitionDepth > 0 || readPersistedUntil() > 0;
}

function subscribe(listener: () => void) {
  listeners.add(listener);
  return () => listeners.delete(listener);
}

export function useCriticalDeviceTransitionActive() {
  return useSyncExternalStore(
    subscribe,
    isCriticalDeviceTransitionActive,
    () => false
  );
}
