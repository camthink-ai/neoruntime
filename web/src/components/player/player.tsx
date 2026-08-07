import { useState, useEffect, useRef, useCallback } from 'react';
import { VideoStreamPlayer } from '@/lib/videoStream';
import { AudioPlayer } from '@/lib/audioStream/audioPlayer';
import Loading from '@/components/loading';
import { Button } from '@/components/ui/button';
import PlayerPanel from './player-panel';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { setItem, getItem } from '@/utils/storage';
import { useAudioControlStore } from '@/store/audio';
import { useAudioTalk } from '@/lib/audioStream/useAudioTalk';
import { WifiOff } from 'lucide-react';
import {
  resolvePlayerObjectFit,
  type PlayerObjectFit,
} from './objectFit';

type PlayerProps = {
  videoUrl: string;
  videoRendererInstance: React.RefObject<VideoStreamPlayer | null>;
  showPanel?: boolean;
  enableDoubleClickFullscreen?: boolean;
  /** Configured stream width for accurate snapshot dimensions */
  streamWidth?: number;
  /** Configured stream height for accurate snapshot dimensions */
  streamHeight?: number;
  /** How the video fills its container */
  objectFit?: PlayerObjectFit;
  /** When false, audio stream is never started and controls are hidden */
  enableAudio?: boolean;
  /** Initial stream stats overlay visibility (overrides localStorage when set) */
  defaultStreamStatsVisible?: boolean;
  /** Show an encoder configuration progress state over the player */
  configuring?: boolean;
  /** Actual decoded frame size, used by overlay layers to match the video. */
  onFrameSizeChange?: (size: { width: number; height: number }) => void;
  /**
   * Render the audio controls (monitor / volume / talk) inside the player
   * overlay and host the audio engine here. Pass true on the single player
   * instance that owns audio (Media page) — other players stay engine-free.
   */
  audioOverlay?: boolean;
};

const STREAM_STATS_KEY = 'deviceToolStreamStatsVisible';
const TRANSFORM_STREAM_RESUME_TIMEOUT = 15000;

export default function Player({
  videoUrl,
  videoRendererInstance,
  showPanel = true,
  enableDoubleClickFullscreen = true,
  streamWidth,
  streamHeight,
  objectFit = 'contain',
  enableAudio = true,
  defaultStreamStatsVisible,
  configuring = false,
  onFrameSizeChange,
  audioOverlay = false,
}: PlayerProps) {
  const [loading, setLoading] = useState(false);
  const { t } = useTranslation();
  // isloading and isReload are mutually exclusive
  // const [isReload, setIsReload] = useState(false);
  const [isShowPanel, setIsShowPanel] = useState(true);
  const [isFullscreen, setIsFullscreen] = useState(false);
  const videoRef = useRef<HTMLVideoElement>(null);
  const containerRef = useRef<HTMLDivElement>(null);
  const idleTimerRef = useRef<number | null>(null);
  const audioPlayerRef = useRef<AudioPlayer | null>(null);
  const lastFrameSizeRef = useRef<{ width: number; height: number } | null>(
    null
  );
  // True while we're waiting out a profile-switch pipeline restart. Suppresses
  // the "Disconnected" UI that would otherwise flash when the daemon tears down
  // its GStreamer pipeline before the player reconnects.
  const profileSwitchingRef = useRef(false);
  const [isShowStreamStats, setIsShowStreamStats] = useState<boolean>(() => {
    if (defaultStreamStatsVisible !== undefined) {
      return defaultStreamStatsVisible;
    }
    if (typeof window === 'undefined') return false;
    try {
      const stored = window.localStorage.getItem(STREAM_STATS_KEY);
      return stored === 'true';
    } catch {
      return false;
    }
  });
  const [streamStats, setStreamStats] = useState({
    fps: 0,
    latency: 0,
    bandwidth: 0,
  });
  const [isControlPanel, setIsControlPanel] = useState(false);
  // Audio controls (monitor / volume / talk) render in the player overlay on
  // pages that opt in via `audioOverlay`. They bridge through this shared
  // store; the player owns the AudioPlayer listen lifecycle (gesture unlock +
  // video-reconnect sync) and, when overlay is on, hosts the click-to-toggle
  // talk engine too.
  const muted = useAudioControlStore(s => s.muted);
  const volume = useAudioControlStore(s => s.volume);
  const captureAvailable = useAudioControlStore(s => s.captureAvailable);
  const setVolume = useAudioControlStore(s => s.setVolume);
  // Browser-side listen intent (pure software: starts/stops the AudioPlayer WS
  // stream). Does NOT touch device capture — /audio/capture/* is owned by the
  // Peripheral page. Driven in tandem with `muted` by `toggleMonitor` (the
  // speaker icon), so the two always read as one coherent on/off.
  const listenEnabled = useAudioControlStore(s => s.listenEnabled);
  const toggleMonitor = useAudioControlStore(s => s.toggleMonitor);
  const enableMonitor = useAudioControlStore(s => s.enableMonitor);
  const playbackEnabled = useAudioControlStore(s => s.playbackEnabled);

  // useAudioTalk owns the click-to-toggle talk engine (PC mic → device
  // speaker) + device audio-status sync (captureAvailable / playbackEnabled),
  // refreshed on mount, window-focus, and cross-page Peripheral events.
  const { talking, level, toggleTalk } = useAudioTalk({
    enabled: audioOverlay,
  });

  // AudioPlayer should run when: audio enabled at all, the user asked to
  // listen, the device capture HW is on, and the browser isn't muted.
  const audioShouldPlay =    enableAudio && listenEnabled && captureAvailable && !muted;

  const [connectionState, setConnectionState] = useState<
    'connecting' | 'connected' | 'disconnected'
  >('connecting');
  const [videoVisible, setVideoVisible] = useState(false);
  const [videoDimensions, setVideoDimensions] = useState({
    width: streamWidth ?? 0,
    height: streamHeight ?? 0,
  });
  const [isTransformUpdating, setIsTransformUpdating] = useState(false);
  const transformRequestCountRef = useRef(0);
  const transformWaitingForDataRef = useRef(false);
  const transformResumeTimerRef = useRef<number | null>(null);

  // Ref mirror so async/gesture callbacks read the latest gate without being
  // recreated on every store update.
  const audioSettingsRef = useRef({ audioShouldPlay, volume, videoUrl });
  audioSettingsRef.current = { audioShouldPlay, volume, videoUrl };

  const startAudioPlayer = useCallback(() => {
    const settings = audioSettingsRef.current;
    if (
      !settings.audioShouldPlay
      || !settings.videoUrl
      || audioPlayerRef.current?.active
    ) {
      return;
    }

    const audioBaseUrl = window.location.origin.replace(/^http/, 'ws');
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    const audioUrl = `${audioBaseUrl}/api/v1/audio/stream?token=${encodeURIComponent(token)}`;
    const player = new AudioPlayer({ syncDelayMs: 300 });
    player.setVolume(settings.volume);
    audioPlayerRef.current = player;
    player.start(audioUrl);
  }, []);

  // Start/stop the AudioPlayer reactively as the store's gate conditions
  // change (mic-listen toggled, mute toggled, capture HW flipped by the
  // Peripheral page). Audio starts on mount — no click required — because
  // in-app navigation here already granted the browser's sticky user
  // activation, so the AudioContext created in startAudioPlayer can resume
  // immediately. The gesture listener below is only a fallback for a cold
  // page-load with no prior activation (e.g. a hard refresh directly onto the
  // Media URL, which the browser's autoplay policy genuinely blocks).
  useEffect(() => {
    if (!audioShouldPlay) {
      if (audioPlayerRef.current) {
        audioPlayerRef.current.stop();
        audioPlayerRef.current = null;
      }
      return;
    }
    startAudioPlayer();
  }, [audioShouldPlay, startAudioPlayer]);

  // Apply browser-side volume changes from the store to the live AudioPlayer.
  useEffect(() => {
    audioPlayerRef.current?.setVolume(volume);
  }, [volume]);

  // Keep snapshot dimensions in a ref to avoid closure staleness in async effects
  const snapshotDimsRef = useRef<{ width: number; height: number } | null>(
    null
  );

  useEffect(() => {
    if (streamWidth && streamHeight) {
      setVideoDimensions({ width: streamWidth, height: streamHeight });
      snapshotDimsRef.current = { width: streamWidth, height: streamHeight };
      videoRendererInstance.current?.setSnapshotDimensions(
        streamWidth,
        streamHeight
      );
    }
  }, [streamWidth, streamHeight, videoRendererInstance]);

  const publishFrameSize = useCallback((width: number, height: number) => {
    if (!width || !height) return;
    const last = lastFrameSizeRef.current;
    if (last?.width === width && last?.height === height) return;
    lastFrameSizeRef.current = { width, height };
    onFrameSizeChange?.({ width, height });
  }, [onFrameSizeChange]);

  useEffect(() => {
    const video = videoRef.current;
    if (!video) return;

    const syncFromVideo = () => {
      publishFrameSize(video.videoWidth, video.videoHeight);
    };
    const syncFromDecoder = (event: Event) => {
      const { detail } = event as CustomEvent<{
        width?: number;
        height?: number;
      }>;
      publishFrameSize(detail?.width ?? 0, detail?.height ?? 0);
    };

    video.addEventListener('loadedmetadata', syncFromVideo);
    video.addEventListener('resize', syncFromVideo);
    video.addEventListener('aipc:video-frame-size', syncFromDecoder);
    const timer = window.setInterval(syncFromVideo, 1000);
    syncFromVideo();

    return () => {
      video.removeEventListener('loadedmetadata', syncFromVideo);
      video.removeEventListener('resize', syncFromVideo);
      video.removeEventListener('aipc:video-frame-size', syncFromDecoder);
      window.clearInterval(timer);
    };
  }, [publishFrameSize]);

  useEffect(() => {
    if (typeof window === 'undefined') return;
    try {
      setItem(STREAM_STATS_KEY, isShowStreamStats);
    } catch {
      // ignore storage errors
    }
  }, [isShowStreamStats]);

  useEffect(() => {
    let isCancelled = false;

    const initializePlayer = async () => {
      setLoading(true);
      setConnectionState('connecting');
      setVideoVisible(false);

      if (!videoUrl) {
        setLoading(false);
        setIsControlPanel(false);
        setConnectionState('disconnected');
        return;
      }

      const video = videoRef.current;
      if (!video) {
        setLoading(false);
        setConnectionState('disconnected');
        return;
      }

      // Brief delay to let browser settle after previous cleanup
      await new Promise<void>(resolve => {
        setTimeout(resolve, 100);
      });
      if (isCancelled) return;

      videoRendererInstance.current = new VideoStreamPlayer();
      videoRendererInstance.current.initPlayer(video);
      const dims = snapshotDimsRef.current;
      if (dims) {
        videoRendererInstance.current.setSnapshotDimensions(
          dims.width,
          dims.height
        );
      }
      videoRendererInstance.current.start(videoUrl);

      // Keep audio active across video stream reconnects.
      if (audioSettingsRef.current.audioShouldPlay) {
        startAudioPlayer();
      }
    };

    initializePlayer();

    return () => {
      isCancelled = true;
      if (videoRendererInstance.current) {
        videoRendererInstance.current.destroy();
        videoRendererInstance.current = null;
      }
      // Stop audio when video disconnects
      if (audioPlayerRef.current) {
        audioPlayerRef.current.stop();
        audioPlayerRef.current = null;
      }
    };
  }, [videoUrl]);

  // Autoplay fallback. Audio starts on mount in the effect above, but if this
  // is a cold page-load with no prior user activation in the document the
  // AudioContext is created suspended and the browser blocks resume() until
  // the first interaction. Resume it on the first pointer/keyboard event
  // anywhere so even that edge case gets sound with a single incidental
  // gesture. The normal in-app-navigation flow already has sticky activation,
  // so the context starts running and this listener is a no-op.
  useEffect(() => {
    if (!enableAudio) return;
    const resumeAudio = () => {
      audioPlayerRef.current?.resume();
      window.removeEventListener('pointerdown', resumeAudio, true);
      window.removeEventListener('keydown', resumeAudio, true);
    };
    window.addEventListener('pointerdown', resumeAudio, {
      capture: true,
      once: true,
    });
    window.addEventListener('keydown', resumeAudio, {
      capture: true,
      once: true,
    });
    return () => {
      window.removeEventListener('pointerdown', resumeAudio, true);
      window.removeEventListener('keydown', resumeAudio, true);
    };
  }, [enableAudio]);

  // Default the browser listen state to ON when the audio-owning player
  // mounts ("喇叭默认开启"). The store is a module singleton that survives
  // route changes; without resetting, the speaker could re-enter in an
  // inconsistent state. With autoplay-on-mount, this intent immediately
  // starts the AudioPlayer (the reactive effect above) — no click needed for
  // the normal in-app-navigation flow. The speaker button is a plain toggle
  // (toggleMonitor) since there's no unlock click to guard against anymore.
  useEffect(() => {
    if (!audioOverlay) return;
    enableMonitor();
  }, [audioOverlay, enableMonitor]);

  // Update stream stats once per second — but ONLY while the stats overlay is
  // shown. The 1 Hz setState re-renders the whole Player subtree and competes
  // with the per-frame decode/append work on the main thread, so on pages that
  // never show stats (e.g. /image, which passes showPanel={false}) we keep the
  // interval off entirely instead of churning every second of playback.
  useEffect(() => {
    if (!isShowStreamStats) return;
    const interval = setInterval(() => {
      const packetsPerSecond =        videoRendererInstance.current?.packetsPerSecond ?? 0;
      const currentPackets = videoRendererInstance.current?.packetCount ?? 0;
      const fps = packetsPerSecond || currentPackets;

      const stats = videoRendererInstance.current?.getStats?.();
      const latency = stats?.latency ?? 0;
      const bandwidth = stats?.bandwidth ?? 0;

      setStreamStats({ fps, latency, bandwidth });
    }, 1000);
    return () => clearInterval(interval);
  }, [isShowStreamStats]);

  const handleReload = () => {
    videoRendererInstance.current?.resetStartState().start(videoUrl);
    setLoading(false);
  };

  // Keep latest handleReload in a ref so the global reload listener (subscribed
  // once) always invokes the current closure with the up-to-date videoUrl.
  const handleReloadRef = useRef(handleReload);
  handleReloadRef.current = handleReload;

  // External callers (e.g. encoder reconfigure) request a full player reconnect
  // by dispatching 'player-reload'. This tears down WS + MSE and reconnects,
  // fetching fresh SPS/PPS and waiting for a clean IDR — avoiding 花屏/黑屏
  // after a resolution/codec/fps change.
  useEffect(() => {
    const onPlayerReload = () => {
      handleReloadRef.current?.();
    };
    window.addEventListener('player-reload', onPlayerReload);
    return () => window.removeEventListener('player-reload', onPlayerReload);
  }, []);

  // AI ISP profile switches restart the daemon's GStreamer pipeline for
  // ~interrupt_ms (reported by the switch RPC). Reconnecting immediately would
  // hit a half-rebuilt pipeline and 花屏/黑屏. Wait out the interrupt window,
  // then reuse the canonical reload path. The switching ref suppresses the
  // transient "Disconnected" state the WS close produces during the wait.
  useEffect(() => {
    let fallback: number | undefined;
    const onProfileChanged = (e: Event) => {
      const { detail } = e as CustomEvent<{ interrupt_ms?: number }>;
      const wait = Math.max(0, detail?.interrupt_ms ?? 0);
      profileSwitchingRef.current = true;
      setLoading(true);
      setConnectionState('connecting');
      setVideoVisible(false);
      // Safety net: if the reconnect never produces a wv_work, drop the guard
      // so the disconnected UI is reachable again.
      fallback = window.setTimeout(() => {
        profileSwitchingRef.current = false;
      }, wait + 10000);
      window.setTimeout(() => {
        handleReloadRef.current?.();
      }, wait + 300);
    };
    window.addEventListener('aipc:media-profile-changed', onProfileChanged);
    return () => {
      window.removeEventListener(
        'aipc:media-profile-changed',
        onProfileChanged
      );
      if (fallback) window.clearTimeout(fallback);
    };
  }, []);

  useEffect(() => {
    const clearTransformResumeTimer = () => {
      if (transformResumeTimerRef.current !== null) {
        window.clearTimeout(transformResumeTimerRef.current);
        transformResumeTimerRef.current = null;
      }
    };

    const onTransformUpdating = (event: Event) => {
      const { detail } = event as CustomEvent<{ active?: boolean }>;
      if (detail?.active === true) {
        transformRequestCountRef.current += 1;
        transformWaitingForDataRef.current = true;
        clearTransformResumeTimer();
        setIsTransformUpdating(true);
        return;
      }

      transformRequestCountRef.current = Math.max(
        0,
        transformRequestCountRef.current - 1
      );
      if (transformRequestCountRef.current > 0) return;

      const player = videoRendererInstance.current;
      if (!player) {
        transformWaitingForDataRef.current = false;
        setIsTransformUpdating(false);
        return;
      }

      player.waitForNextVideoData();
      clearTransformResumeTimer();
      transformResumeTimerRef.current = window.setTimeout(() => {
        transformWaitingForDataRef.current = false;
        setIsTransformUpdating(false);
        setLoading(false);
        setVideoVisible(false);
        setConnectionState('disconnected');
      }, TRANSFORM_STREAM_RESUME_TIMEOUT);
    };

    window.addEventListener(
      'aipc:media-transform-updating',
      onTransformUpdating
    );
    return () => {
      window.removeEventListener(
        'aipc:media-transform-updating',
        onTransformUpdating
      );
      clearTransformResumeTimer();
    };
  }, [videoRendererInstance]);

  useEffect(() => {
    const handlWvClose = (e: Event) => {
      const event = e as CustomEvent<{ code?: number; reason?: string }>;
      if (event.detail.reason === 'Connection replaced') {
        toast.error(t('sys.device_tool.preview_disconnected'));
      }
      // During a profile-switch pipeline restart the WS close is expected; keep
      // the loading spinner up so the user doesn't see a disconnected flicker.
      if (profileSwitchingRef.current) return;
      setIsControlPanel(false);
      setLoading(false);
      setConnectionState('disconnected');
    };

    const handleWvWork = (e: Event) => {
      const isWorking = (e as CustomEvent<boolean>).detail;
      setLoading(!isWorking);
      if (isWorking) {
        if (
          transformWaitingForDataRef.current
          && transformRequestCountRef.current === 0
        ) {
          transformWaitingForDataRef.current = false;
          if (transformResumeTimerRef.current !== null) {
            window.clearTimeout(transformResumeTimerRef.current);
            transformResumeTimerRef.current = null;
          }
          setIsTransformUpdating(false);
        }
        // Reconnect landed after a profile switch — drop the guard so later
        // genuine disconnects surface normally.
        profileSwitchingRef.current = false;
        setIsControlPanel(true);
        setConnectionState('connected');
        setVideoVisible(true);
      } else {
        setIsControlPanel(false);
      }
    };
    const handleWvError = (_e: Event) => {
      // ignore websocket errors
    };

    window.addEventListener('wv_work', handleWvWork);
    window.addEventListener('wv_close', handlWvClose);
    window.addEventListener('wv_error', handleWvError);
    return () => {
      window.removeEventListener('wv_work', handleWvWork);
      window.removeEventListener('wv_close', handlWvClose);
      window.removeEventListener('wv_error', handleWvError);
    };
  }, [t]);

  const handleSnapshot = () => {
    videoRendererInstance.current?.doSnapshot();
  };
  const handleFullscreen = useCallback(async () => {
    try {
      if (!document.fullscreenElement) {
        await containerRef.current?.requestFullscreen?.();
      } else {
        await document.exitFullscreen();
      }
    } catch {
      setIsFullscreen(false);
    }
  }, []);
  useEffect(() => {
    const onFsChange = () => {
      const isFs = Boolean(document.fullscreenElement);
      setIsFullscreen(isFs);
    };
    document.addEventListener('fullscreenchange', onFsChange);
    return () => document.removeEventListener('fullscreenchange', onFsChange);
  }, []);

  // Double-click to fullscreen
  useEffect(() => {
    if (!enableDoubleClickFullscreen) return;

    const el = videoRef.current;
    if (!el) return;
    const onDbl = () => {
      handleFullscreen().catch(() => {
        // ignore fullscreen errors
      });
    };
    el.addEventListener('dblclick', onDbl);
    return () => {
      el.removeEventListener('dblclick', onDbl);
    };
  }, [enableDoubleClickFullscreen, handleFullscreen]);

  // Hide panel on mouse enter
  useEffect(() => {
    const el = containerRef.current;
    if (!el) return;
    const clearIdle = () => {
      if (idleTimerRef.current !== null) {
        clearTimeout(idleTimerRef.current);
        idleTimerRef.current = null;
      }
    };
    const startIdle = () => {
      clearIdle();
      idleTimerRef.current = window.setTimeout(() => {
        setIsShowPanel(false);
      }, 3000);
    };
    const onEnter = () => {
      setIsShowPanel(true);
      startIdle();
    };
    const onLeave = () => {
      setIsShowPanel(false);
      clearIdle();
    };
    const onMove = () => {
      setIsShowPanel(true);
      startIdle();
    };
    el.addEventListener('mouseenter', onEnter);
    el.addEventListener('mouseleave', onLeave);
    el.addEventListener('mousemove', onMove);
    return () => {
      el.removeEventListener('mouseenter', onEnter);
      el.removeEventListener('mouseleave', onLeave);
      el.removeEventListener('mousemove', onMove);
      clearIdle();
    };
  }, []);

  const syncVideoDimensions = () => {
    const video = videoRef.current;
    if (!video || video.videoWidth <= 0 || video.videoHeight <= 0) return;
    setVideoDimensions(current => (current.width === video.videoWidth && current.height === video.videoHeight
        ? current
        : { width: video.videoWidth, height: video.videoHeight }));
  };

  const resolvedObjectFit = resolvePlayerObjectFit(
    objectFit,
    videoDimensions.width,
    videoDimensions.height
  );

  return (
    <div className="w-full h-full">
      <div
        ref={containerRef}
        className="relative w-full h-full flex items-center justify-center overflow-hidden bg-black"
      >
        <div className="w-full h-full flex items-center justify-center">
          <video
            ref={videoRef}
            className={`h-full w-full ${resolvedObjectFit === 'cover' ? 'object-cover' : 'object-contain'} transition-opacity duration-500 ${videoVisible ? 'opacity-100' : 'opacity-0'}`}
            id="videoPlayer"
            muted
            playsInline
            autoPlay
            onLoadedMetadata={syncVideoDimensions}
            onResize={syncVideoDimensions}
            disableRemotePlayback
          />
          {isShowStreamStats && (
            <div className="absolute md:top-4 top-2 md:right-4 right-2 bg-gray-800/50 px-2 py-1 rounded text-xs font-mono space-y-0.5">
              <div>
                <span className="text-gray-400 font-bold">
                  {t('sys.device_tool.fps')}:
                </span>{' '}
                <span className="text-white">{streamStats.fps}</span>
              </div>
              <div>
                <span className="text-gray-400 font-bold">
                  {t('sys.device_tool.latency')}:
                </span>{' '}
                <span className="text-white">
                  {streamStats.latency.toFixed(2)}s
                </span>
              </div>
              <div>
                <span className="text-gray-400 font-bold">
                  {t('sys.device_tool.bandwidth')}:
                </span>{' '}
                <span className="text-white">
                  {streamStats.bandwidth.toFixed(1)} kB/s
                </span>
              </div>
            </div>
          )}
        </div>
        {showPanel && (
          <PlayerPanel
            className={`absolute bottom-0 left-0 transition-all duration-300 ease-in-out ${
              isShowPanel
                ? 'opacity-100 translate-y-0'
                : 'opacity-0 translate-y-full'
            }`}
            isFullscreen={isFullscreen}
            snapshot={handleSnapshot}
            fullscreen={handleFullscreen}
            isControlPanel={isControlPanel}
            isShowStreamStats={isShowStreamStats}
            toggleStreamStats={() => setIsShowStreamStats(prev => !prev)}
            audioOverlay={audioOverlay}
            isAudioMuted={muted}
            onToggleMonitor={toggleMonitor}
            audioVolume={volume}
            onVolumeChange={setVolume}
            captureAvailable={captureAvailable}
            talking={talking}
            talkLevel={level}
            onToggleTalk={toggleTalk}
            playbackEnabled={playbackEnabled}
          />
        )}
        {(loading || configuring || isTransformUpdating) && (
          <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 flex flex-col items-center gap-2">
            <Loading
              fullHeight={false}
              className="w-40"
              placeholder={
                isTransformUpdating
                  ? t(
                      'sys.media_player.transform_updating',
                      'Updating image...'
                    )
                  : configuring
                    ? t('sys.media_settings.configuring', 'Configuring...')
                    : undefined
              }
            />
          </div>
        )}
        {!loading
          && !configuring
          && !isTransformUpdating
          && connectionState === 'disconnected' && (
            <div className="absolute left-1/2 top-1/2 -translate-x-1/2 -translate-y-1/2 flex flex-col items-center gap-3">
              <WifiOff
                aria-hidden="true"
                className="size-10 text-white/70"
                strokeWidth={1.5}
              />
              <span className="text-sm text-white/70">
                {t('sys.device_tool.disconnected', 'Disconnected')}
              </span>
              {videoUrl && (
                <Button
                  onClick={handleReload}
                  size="sm"
                  variant="ghost"
                  className="bg-white/20 hover:bg-white/30 text-white hover:text-white"
                >
                  {t('sys.device_tool.reload')}
                </Button>
              )}
            </div>
          )}
      </div>
    </div>
  );
}
