import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { AudioTalker } from './audioTalker';
import { useAudioControlStore } from '@/store/audio';
import { fetchAudioStatus, audioTalkStreamUrl } from '@/services/settings';

/**
 * useAudioTalk — click-to-toggle talk engine + device audio-status sync for the
 * player overlay. Extracted from the old Media-sidebar `AudioBlock` so the
 * overlay (`player-panel`) can render the controls while a single hook owns the
 * AudioTalker WebSocket + getUserMedia lifecycle, the click-to-start /
 * click-to-stop toggle semantics with a double-click guard, and the
 * cross-page capture/playback gate refresh.
 *
 * Pass `enabled = true` only on the player instance that hosts audio (Media
 * page). When false every effect short-circuits — no status fetch, no event
 * listeners, no AudioTalker — so non-audio players (dashboard / image) stay
 * cheap and never spawn a second talk session.
 *
 * Gate flags (`captureAvailable` / `playbackEnabled`) are written to the shared
 * audio store so `player.tsx` (the AudioPlayer listen host) and the overlay
 * buttons read one source of truth. Talk is additionally gated by
 * `playbackEnabled` (the Peripheral speaker-output switch): the browser mic
 * can only feed a device that is driving its speaker.
 */
export function useAudioTalk({ enabled }: { enabled: boolean }) {
  const { t } = useTranslation();

  const setCaptureAvailable = useAudioControlStore(s => s.setCaptureAvailable);
  const setPlaybackEnabled = useAudioControlStore(s => s.setPlaybackEnabled);
  const playbackEnabled = useAudioControlStore(s => s.playbackEnabled);

  const [talking, setTalking] = useState(false);
  const [level, setLevel] = useState(0);

  const talkerRef = useRef<AudioTalker | null>(null);
  const stopGuardRef = useRef(false); // ensures teardown runs once per session

  // Ref mirror so the setTimeout-fired startTalk reads the latest gate without
  // being recreated on every store update.
  const playbackRef = useRef(playbackEnabled);
  playbackRef.current = playbackEnabled;

  // Pull device capture/playback status into the store on mount + window focus
  // (so Peripheral-page toggles are reflected when the user returns).
  const loadData = useCallback(async () => {
    try {
      const audioStatus = await fetchAudioStatus();
      setCaptureAvailable(!!audioStatus.capturing);
      setPlaybackEnabled(
        audioStatus.playback_enabled === undefined
          ? true
          : !!audioStatus.playback_enabled
      );
    } catch {
      // API errors handled by the request interceptor.
    }
  }, [setCaptureAvailable, setPlaybackEnabled]);

  useEffect(() => {
    if (!enabled) return;
    loadData();
    const onFocus = () => loadData();
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, [enabled, loadData]);

  // Cross-page real-time sync: Peripheral dispatches these after its toggles.
  useEffect(() => {
    if (!enabled) return;
    const onCaptureChange = (e: Event) => setCaptureAvailable(!!(e as CustomEvent<boolean>).detail);
    const onPlaybackChange = (e: Event) => setPlaybackEnabled(!!(e as CustomEvent<boolean>).detail);
    window.addEventListener('audio-capture-change', onCaptureChange);
    window.addEventListener('audio-playback-change', onPlaybackChange);
    return () => {
      window.removeEventListener('audio-capture-change', onCaptureChange);
      window.removeEventListener('audio-playback-change', onPlaybackChange);
    };
  }, [enabled, setCaptureAvailable, setPlaybackEnabled]);

  const finishTalk = useCallback(async () => {
    if (stopGuardRef.current) return;
    stopGuardRef.current = true;
    setTalking(false);
    setLevel(0);
    try {
      // Await the WebSocket close so the server-side StreamAudioPcm teardown
      // (and camera-daemon stop_playback) completes cleanly. See audioTalker.stop().
      await talkerRef.current?.stop();
    } catch {
      /* ignore */
    }
  }, []);

  // One AudioTalker for the player's lifetime, wired to UI callbacks.
  useEffect(() => {
    if (!enabled) return;
    talkerRef.current = new AudioTalker({
      onLevel: l => setLevel(l),
      onBusy: () => {
        toast.error(
          t('sys.media_settings.talk_busy', 'Another talk session is active')
        );
        setTalking(false);
        finishTalk().catch(() => {});
      },
      onError: msg => {
        toast.error(
          `${t('sys.media_settings.talk_error', 'Talk error')}: ${msg}`
        );
        finishTalk().catch(() => {});
      },
    });
    return () => {
      talkerRef.current?.stop();
      talkerRef.current = null;
    };
  }, [enabled, t, finishTalk]);

  const startTalk = useCallback(async () => {
    if (!playbackRef.current || !talkerRef.current) return;
    stopGuardRef.current = false;
    setLevel(0);
    try {
      await talkerRef.current.start(audioTalkStreamUrl());

      // User released the button before start finished -> tear straight down.
      if (stopGuardRef.current) {
        talkerRef.current.stop();
        return;
      }
      if (!talkerRef.current.active) {
        // Busy / rejected — nothing to restore (full-duplex: capture untouched).
        return;
      }
      setTalking(true);
    } catch (err) {
      if (err instanceof DOMException) {
        const microphoneErrorKeys: Record<string, [string, string]> = {
          NotAllowedError: [
            'sys.media_settings.mic_permission_denied',
            'Microphone permission denied',
          ],
          PermissionDeniedError: [
            'sys.media_settings.mic_permission_denied',
            'Microphone permission denied',
          ],
          NotFoundError: [
            'sys.media_settings.mic_not_found',
            'No microphone was detected. Connect one and try again.',
          ],
          DevicesNotFoundError: [
            'sys.media_settings.mic_not_found',
            'No microphone was detected. Connect one and try again.',
          ],
          NotReadableError: [
            'sys.media_settings.mic_not_readable',
            'The microphone is unavailable. Check whether another app is using it.',
          ],
          TrackStartError: [
            'sys.media_settings.mic_not_readable',
            'The microphone is unavailable. Check whether another app is using it.',
          ],
          SecurityError: [
            'sys.media_settings.mic_security_error',
            'Microphone access is blocked by the browser security settings.',
          ],
          AbortError: [
            'sys.media_settings.mic_start_aborted',
            'Microphone startup was interrupted. Please try again.',
          ],
        };
        const localizedError = microphoneErrorKeys[err.name];
        if (localizedError) {
          toast.error(t(...localizedError));
          return;
        }
      }

      const detail = err instanceof Error ? err.message : String(err ?? '');
      toast.error(
        `${t('sys.media_settings.talk_error', 'Talk error')}${detail ? `: ${detail}` : ''}`
      );
    }
  }, [t]);

  // Click-to-toggle: one click starts a talk session, the next click ends it.
  // `startingRef` guards the async start window so a double-click (or a rapid
  // second tap while getUserMedia is still resolving) doesn't open two
  // sessions — a tap during the start window is treated as "stop".
  const startingRef = useRef(false);
  const toggleTalk = useCallback(() => {
    if (!enabled || !playbackRef.current || !talkerRef.current) return;
    if (talking || startingRef.current) {
      finishTalk().catch(() => {});
      return;
    }
    startingRef.current = true;
    startTalk().finally(() => {
      startingRef.current = false;
    });
  }, [enabled, talking, startTalk, finishTalk]);

  return { talking, level, toggleTalk };
}
