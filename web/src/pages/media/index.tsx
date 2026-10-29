import { useCallback, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle, X } from 'lucide-react';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Player from '@/components/player/player';
import MediaSettings from './components/MediaSettings';
import { getItem } from '@/utils/storage';
import { useMediaStatus } from './hooks/useMedia';
import { useStreamFallback, useHevcNoticeDismissal } from '@/hooks/useStreamFallback';

export default function Monitoring() {
  const { t } = useTranslation();
  const playerRef = useRef<VideoStreamPlayer | null>(null);

  const [activeStream, setActiveStream] = useState<'main' | 'sub' | 'third'>(
    'main'
  );
  const [isEncoderConfiguring, setIsEncoderConfiguring] = useState(false);
  // 用户手动关闭"浏览器不支持 H.265"提示条（localStorage 持久化，全局共享）
  const [noticeDismissed, dismissNotice] = useHevcNoticeDismissal();
  const [isEncoderRestarting, setIsEncoderRestarting] = useState(false);

  const { data: mediaStatus } = useMediaStatus();

  const streams = mediaStatus?.streams ?? [];

  const { effectiveStreamId, effectiveStreamInfo, codecNotice } =    useStreamFallback({ streams, activeStream });

  const activeStreamInfo = effectiveStreamInfo;

  const handleStreamChange = (streamId: string) => {
    if (streamId === 'main' || streamId === 'sub' || streamId === 'third') {
      setActiveStream(streamId);
    }
  };

  // Whether the stream we'd actually feed to the player is live. Mirrors the
  // `enabled` flag used by MediaSettings (`has_encoder`). Keying off has_encoder
  // rather than status === 'active' avoids briefly blanking the player when
  // another stream is disabled: the backend rebuilds the remaining encoders,
  // which transiently report status 'starting' (no first frame yet) but still
  // have has_encoder === true.
  const effectiveEnabled = !!effectiveStreamInfo
    && !!effectiveStreamInfo.has_encoder;

  // Build a fresh authenticated WebSocket URL for the selected stream.
  const buildStreamUrl = useCallback((streamId: string) => {
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    const baseUrl = window.location.origin.replace(/^http/, 'ws');
    return `${baseUrl}/api/v1/h264/${streamId}?token=${encodeURIComponent(token)}`;
  }, []);

  // Incrementing the generation guarantees a different URL and a full Player
  // remount after every successful enable/disable operation.
  const [playerKey, setPlayerKey] = useState(0);
  // Full codec/resolution changes rebuild the backend pipeline. Disconnect the
  // old player immediately and reconnect only after the mutation's status poll
  // completes, using the final effective stream (including H.265 fallback).
  const currentStreamUrl = effectiveEnabled && !isEncoderRestarting
    ? `${buildStreamUrl(effectiveStreamId)}&reconnect=${playerKey}`
    : '';

  const handleStreamToggleSuccess = useCallback((_streamId: string) => {
    // A stream layout change recreates every encoder, even when the toggled
    // stream is not the one currently displayed. Always give the active
    // decoder a fresh URL/generation after the rebuild.
    setPlayerKey(key => key + 1);
  }, []);

  const noticeText = codecNotice
    ? t(
        `sys.monitoring.${codecNotice}`,
        codecNotice === 'hevc_unsupported_fallback'
          ? '当前浏览器不支持 H.265，请切换到 Chrome 或 Safari 浏览器'
          : '当前浏览器不支持 H.265，且无可用 H.264 码流，请切换到 Chrome 或 Safari 浏览器'
      )
    : null;

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-background md:flex-row">
      {/* Main Content Area */}
      <div className="flex w-full shrink-0 flex-col items-center justify-center pt-2 pb-1 md:h-full md:min-h-0 md:flex-1 md:shrink px-4 md:py-0">
        {noticeText && !noticeDismissed && (
          <div className="flex w-full items-center gap-1.5 rounded-md bg-amber-500/10 px-3 py-1.5 text-xs text-amber-600 dark:text-amber-400">
            <AlertTriangle className="h-3.5 w-3.5 shrink-0" />
            <span className="flex-1">{noticeText}</span>
            <button
              type="button"
              className="shrink-0 rounded p-0.5 transition-colors hover:bg-amber-500/20"
              onClick={() => dismissNotice()}
              aria-label={t('common.close', '关闭')}
            >
              <X className="h-3.5 w-3.5" />
            </button>
          </div>
        )}
        <div className="relative aspect-video w-full max-w-full overflow-hidden rounded-xl bg-black my-2 md:max-w-none md:rounded-2xl">
          <Player
            videoUrl={currentStreamUrl}
            videoRendererInstance={playerRef}
            streamWidth={activeStreamInfo?.width}
            streamHeight={activeStreamInfo?.height}
            objectFit="adaptive"
            audioOverlay
            configuring={isEncoderConfiguring}
            streamRestarting={isEncoderRestarting}
          />
        </div>
      </div>

      {/* 配置侧栏 — 移动端底部全宽 + 上边框，桌面端右侧固定宽 */}
      <div className="relative z-10 flex min-h-0 flex-1 flex-col overflow-hidden border-t border-border bg-card md:h-full md:w-96 md:shrink-0 md:flex-none md:border-l md:border-t-0">
        <div className="flex px-6 pt-6 pb-2 items-center gap-2 border-b border-border text-lg font-semibold tracking-tight text-foreground">
          <span className="flex items-center gap-2">
            {' '}
            {t('sys.monitoring.config', 'Config')}{' '}
          </span>
        </div>

        {/* Config Content */}
        <div className="relative min-h-0 flex-1 overflow-hidden">
          <MediaSettings
            activeStream={activeStream}
            onStreamChange={handleStreamChange}
            onStreamToggleSuccess={handleStreamToggleSuccess}
            onEncoderConfiguringChange={setIsEncoderConfiguring}
            onEncoderRestartingChange={setIsEncoderRestarting}
          />
        </div>
      </div>
    </div>
  );
}
