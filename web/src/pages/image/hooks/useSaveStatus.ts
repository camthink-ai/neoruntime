import { useCallback, useEffect, useRef, useState } from 'react';

export type SaveStatus = 'idle' | 'saving' | 'saved' | 'error';

export function useSaveStatus() {
  const [saveStatus, setSaveStatus] = useState<SaveStatus>('idle');
  const mountedRef = useRef(true);
  const resetTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    // React Strict Mode runs an extra setup/cleanup cycle in development.
    // Restore the mounted flag in setup so the simulated cleanup does not
    // permanently disable save-state updates.
    mountedRef.current = true;
    return () => {
      mountedRef.current = false;
      if (resetTimerRef.current) clearTimeout(resetTimerRef.current);
    };
  }, []);

  const markSaving = useCallback(() => {
    if (!mountedRef.current) return;
    if (resetTimerRef.current) clearTimeout(resetTimerRef.current);
    setSaveStatus('saving');
  }, []);

  const markSaved = useCallback(() => {
    if (!mountedRef.current) return;
    setSaveStatus('saved');
    resetTimerRef.current = setTimeout(() => {
      if (mountedRef.current) setSaveStatus('idle');
    }, 1500);
  }, []);

  const markError = useCallback(() => {
    if (!mountedRef.current) return;
    if (resetTimerRef.current) clearTimeout(resetTimerRef.current);
    setSaveStatus('error');
  }, []);

  return {
    saveStatus,
    mountedRef,
    markSaving,
    markSaved,
    markError,
  };
}
