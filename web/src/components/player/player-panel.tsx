import { useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import SvgIcon from '@/components/svg-icon';
import { cn } from '@/lib/utils';

type PlayerPanelProps = {
  snapshot: () => void;
  className?: string;
  isFullscreen: boolean;
  fullscreen: () => void;
  isControlPanel?: boolean;
  isShowStreamStats?: boolean;
  toggleStreamStats?: () => void;
  /** Render the audio control group (monitor / volume / talk). */
  audioOverlay?: boolean;
  /** Browser-side mute state (mirrors "monitor off"). */
  isAudioMuted?: boolean;
  /** Click "listen to site audio" toggle — drives monitor on/off. */
  onToggleMonitor?: () => void;
  /** 0..1 browser volume. */
  audioVolume?: number;
  onVolumeChange?: (v: number) => void;
  /** Device capture HW is on — gates the monitor (speaker) switch. */
  captureAvailable?: boolean;
  /** Talk session is live (PC mic → device speaker). */
  talking?: boolean;
  /** 0..1 live PC-mic input level while talking — drives the speaking halo. */
  talkLevel?: number;
  /** Click-to-toggle talk on/off. */
  onToggleTalk?: () => void;
  /** Device playback (speaker) HW is on — gates the talk switch. */
  playbackEnabled?: boolean;
};

export default function PlayerPanel({
  isFullscreen,
  snapshot,
  fullscreen,
  isControlPanel,
  isShowStreamStats,
  toggleStreamStats,
  audioOverlay = false,
  isAudioMuted = true,
  onToggleMonitor,
  audioVolume = 1,
  onVolumeChange,
  captureAvailable = false,
  talking = false,
  talkLevel = 0,
  onToggleTalk,
  playbackEnabled = false,
  className,
}: PlayerPanelProps) {
  const { t } = useTranslation();

  const volumePct = Math.round(Math.max(0, Math.min(1, audioVolume)) * 100);
  // While muted the slider renders at 0% but the real volume is preserved, so
  // unmuting snaps it back to where it was. Any manual adjustment (drag/wheel)
  // also clears mute so the slider never feels stuck at 0.
  const displayPct = isAudioMuted ? 0 : volumePct;

  const handleVolumeChange = (v: number) => {
    onVolumeChange?.(v);
    if (isAudioMuted && v > 0) onToggleMonitor?.();
  };

  // Allow wheel-scrolling anywhere over the volume control (button or slider)
  // to nudge volume up/down. The listener sits on the group wrapper, which is
  // always pointer-events-auto and is the DOM ancestor of both the button and
  // the hover popup, so wheel bubbles to it from either. Native non-passive so
  // preventDefault works (React's synthetic onWheel is passive at the root).
  const volumeWrapRef = useRef<HTMLDivElement>(null);
  const volumeApiRef = useRef({
    audioVolume,
    isAudioMuted,
    onVolumeChange,
    onToggleMonitor,
  });
  volumeApiRef.current = {
    audioVolume,
    isAudioMuted,
    onVolumeChange,
    onToggleMonitor,
  };
  useEffect(() => {
    const el = volumeWrapRef.current;
    if (!el) return;
    const onWheel = (e: WheelEvent) => {
      e.preventDefault();
      const {
        audioVolume: v,
        isAudioMuted: muted,
        onVolumeChange: setVol,
        onToggleMonitor: toggleMonitor,
      } = volumeApiRef.current;
      if (!setVol) return;
      const dir = e.deltaY < 0 ? 1 : -1;
      // Base the step on the displayed value so scrolling up from a muted (0%)
      // state starts from 0, not the remembered volume.
      const base = muted ? 0 : v;
      const next = Math.max(0, Math.min(1, base + dir * 0.05));
      setVol(next);
      if (muted && next > 0) toggleMonitor?.();
    };
    el.addEventListener('wheel', onWheel, { passive: false });
    return () => el.removeEventListener('wheel', onWheel);
  }, []);

  return (
    <div
      className={cn(
        'w-full md:h-[60px] h-[40px] md:px-12 px-8 flex items-center justify-end transition-all duration-300 ease-in-out bg-linear-to-t from-black/70 via-black/30 to-transparent',
        className
      )}
    >
      <div className="flex gap-3">
        {audioOverlay && (
          <>
            {/* Monitor — listen to site audio on/off (device mic → PC speaker)
                with a hover-revealed volume slider. Disabled while the
                Peripheral mic-input switch is off; in that state hovering
                shows the enable hint instead of the (inaudible) slider. */}
            <div
              ref={volumeWrapRef}
              className="group/vol relative md:w-10 w-8 md:h-10 h-8 flex items-center justify-center"
            >
              <button
                disabled={!captureAvailable}
                onClick={onToggleMonitor}
                className="md:w-7 w-6 md:h-7 h-6 flex items-center justify-center disabled:opacity-50 disabled:pointer-events-none"
              >
                {isAudioMuted || volumePct === 0 ? (
                  <SvgIcon
                    icon="audio-close"
                    className="w-full h-full flex-1 text-[#f3f2f3]"
                  />
                ) : (
                  <SvgIcon
                    icon="audio"
                    className="w-full h-full flex-1 text-[#f3f2f3]"
                  />
                )}
              </button>
              {/* Hover popup: vertical slider + % label. pointer-events stays off
                  while hidden (so it never blocks the video area) but flips to
                  auto on group hover — that's what makes the slider draggable
                  AND keeps the popup visible while the cursor is on it, with no
                  dead-zone gap that would hide it mid-hover. Suppressed while
                  capture is off (see capture-off hint below). */}
              <div
                className={cn(
                  'pointer-events-none absolute bottom-full left-1/2 -translate-x-1/2 mb-2 flex flex-col items-center gap-1 rounded-lg bg-black/80 px-2 py-3 opacity-0 transition-opacity',
                  captureAvailable
                    && 'group-hover/vol:pointer-events-auto group-hover/vol:opacity-100'
                )}
              >
                <div className="h-20 w-5 flex items-center justify-center">
                  <input
                    type="range"
                    min={0}
                    max={100}
                    value={displayPct}
                    onChange={e => handleVolumeChange(Number(e.target.value) / 100)}
                    aria-label={t('sys.media_settings.volume', 'Volume')}
                    style={{
                      writingMode: 'vertical-rl',
                      direction: 'rtl',
                      width: '4px',
                      height: '72px',
                    }}
                    className="accent-white cursor-pointer"
                  />
                </div>
                <span className="text-[10px] tabular-nums text-white">
                  {displayPct}%
                </span>
              </div>
              {/* Bridge — fills the mb-2 gap between button top and popup bottom
                  so the cursor can travel up without dropping the hover. */}
              <div
                className={cn(
                  'pointer-events-none absolute bottom-full left-1/2 -translate-x-1/2 h-2 w-10',
                  captureAvailable && 'group-hover/vol:pointer-events-auto'
                )}
              />
              {/* Capture-off hint: shown on hover only while the device mic is
                  off (replaces the suppressed slider). */}
              {!captureAvailable && (
                <span className="pointer-events-none absolute -top-7 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-black/75 px-2 py-0.5 text-[10px] text-white opacity-0 transition-opacity group-hover/vol:opacity-100">
                  {t(
                    'sys.media_settings.mic_listen_capture_off_hint',
                    '请先在外设控制页面开启麦克风输入'
                  )}
                </span>
              )}
            </div>

            {/* Talk — PC mic → device speaker. Click-to-toggle: one click opens
                the talk session, the next closes it. Disabled while the
                Peripheral speaker-output switch is off. */}
            <div className="relative md:w-10 w-8 md:h-10 h-8 flex items-center justify-center group/talk">
              <button
                disabled={!playbackEnabled}
                onClick={onToggleTalk}
                className={cn(
                  'relative md:w-7 w-6 md:h-7 h-6 flex items-center justify-center rounded-full disabled:opacity-50 disabled:pointer-events-none transition-colors',
                  talking ? 'bg-red-600/90' : 'hover:bg-transparent'
                )}
              >
                {/* Speaking halo — only while talking. A ring that expands and
                    brightens with the live PC-mic level so the user can SEE
                    their voice being captured. Bare mic = mic is LIVE; the
                    slashed mic rendered when off = mic is OFF (classic mute
                    semantics). CSS transition smooths per-frame level ticks. */}
                {talking && (
                  <span
                    aria-hidden
                    className="pointer-events-none absolute inset-0 rounded-full border-2 border-red-300"
                    style={{
                      transform: `scale(${1 + Math.min(talkLevel, 1) * 0.5})`,
                      opacity: 0.25 + Math.min(talkLevel, 1) * 0.5,
                      transition: 'transform 80ms linear, opacity 80ms linear',
                    }}
                  />
                )}
                <SvgIcon
                  icon={talking ? 'microphone' : 'microphone-off'}
                  className={cn(
                    'relative w-full h-full flex-1',
                    talking ? 'text-white' : 'text-[#f3f2f3]'
                  )}
                />
              </button>
              <span className="pointer-events-none absolute -top-7 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-black/75 px-2 py-0.5 text-[10px] text-white opacity-0 transition-opacity group-hover/talk:opacity-100">
                {playbackEnabled
                  ? t('sys.media_settings.talk', '对讲')
                  : t(
                      'sys.media_settings.talk_playback_off_hint',
                      '请先在外设控制页面开启扬声器输出'
                    )}
              </span>
            </div>
          </>
        )}

        {/* Stream stats toggle */}
        <div className="relative md:w-10 w-8 md:h-10 h-8 flex items-center justify-center group">
          <button
            disabled={!isControlPanel}
            onClick={toggleStreamStats}
            className="md:w-7 w-6 md:h-7 h-6 disabled:opacity-50 disabled:pointer-events-none"
          >
            {isShowStreamStats ? (
              <SvgIcon
                className="w-full h-full flex-1 text-[#f3f2f3]"
                icon="show_info"
              />
            ) : (
              <SvgIcon
                className="w-full h-full flex-1 text-[#f3f2f3]"
                icon="close_info"
              />
            )}
          </button>
          <span className="pointer-events-none absolute -top-7 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-black/75 px-2 py-0.5 text-[10px] text-white opacity-0 transition-opacity group-hover:opacity-100">
            {t('sys.device_tool.stream_info')}
          </span>
        </div>

        {/* Snapshot */}
        <div className="relative md:w-10 w-8 md:h-10 h-8 flex items-center justify-center group">
          <button
            disabled={!isControlPanel}
            onClick={snapshot}
            className="md:w-7 w-6 md:h-7 h-6 disabled:opacity-50 disabled:pointer-events-none"
          >
            <SvgIcon
              className="w-full h-full flex-1 text-[#f3f2f3]"
              icon="screenshot"
            />
          </button>
          <span className="pointer-events-none absolute -top-7 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-black/75 px-2 py-0.5 text-[10px] text-white opacity-0 transition-opacity group-hover:opacity-100">
            {t('sys.device_tool.snapshot')}
          </span>
        </div>

        {/* Fullscreen */}
        <div className="relative md:w-10 w-8 md:h-10 h-8 flex items-center justify-center group">
          <button
            disabled={!isControlPanel}
            onClick={fullscreen}
            className="md:w-7 w-6 md:h-7 h-6 disabled:opacity-50 disabled:pointer-events-none"
          >
            {isFullscreen ? (
              <SvgIcon
                className="w-full h-full flex-1 text-[#f3f2f3]"
                icon="fullscreen_exit"
              />
            ) : (
              <SvgIcon
                className="w-full h-full flex-1 text-[#f3f2f3]"
                icon="fullscreen"
              />
            )}
          </button>
          <span className="pointer-events-none absolute -top-7 left-1/2 -translate-x-1/2 whitespace-nowrap rounded bg-black/75 px-2 py-0.5 text-[10px] text-white opacity-0 transition-opacity group-hover:opacity-100">
            {t('sys.device_tool.fullscreen')}
          </span>
        </div>
      </div>
    </div>
  );
}
