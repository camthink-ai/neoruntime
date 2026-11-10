import { useCallback, useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle } from 'lucide-react';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Player from '@/components/player/player';
import MediaSettings from './components/MediaSettings';
import { getItem } from '@/utils/storage';
import { useMediaStatus } from './hooks/useMedia';
import { isHevcSupported } from '@/lib/videoStream/hevcSupport';

export default function Monitoring() {
  const { t } = useTranslation();
  const playerRef = useRef<VideoStreamPlayer | null>(null);

  const [activeStream, setActiveStream] = useState<'main' | 'sub' | 'third'>(
    'main'
  );
  const [isEncoderConfiguring, setIsEncoderConfiguring] = useState(false);
  const [isEncoderRestarting, setIsEncoderRestarting] = useState(false);

  const { data: mediaStatus } = useMediaStatus();

  const streams = mediaStatus?.streams ?? [];

  const activeStreamInfo = streams.find(stream => stream.stream_id === activeStream);
  const hevcSupported = useMemo(() => isHevcSupported(), []);
  const [codecMismatch, setCodecMismatch] = useState(false);
  const codec = activeStreamInfo?.codec?.toLowerCase();

  useEffect(() => {
    setCodecMismatch(false);
  }, [activeStream, codec]);

  useEffect(() => {
    const handleError = (event: Event) => {
      const { detail } = (event as CustomEvent<{ type?: string }>);
      if (detail?.type === 'codec_mismatch') setCodecMismatch(true);
    };
    window.addEventListener('wv_error', handleError);
    return () => window.removeEventListener('wv_error', handleError);
  }, []);

  const unsupportedPreview = codec === 'h265' && (!hevcSupported || codecMismatch);

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
  const effectiveEnabled = !!activeStreamInfo
    && !!activeStreamInfo.has_encoder;

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
  // completes, using the selected stream.
  const currentStreamUrl = effectiveEnabled && !isEncoderRestarting && !unsupportedPreview
    ? `${buildStreamUrl(activeStream)}&reconnect=${playerKey}`
    : '';

  const handleStreamToggleSuccess = useCallback((_streamId: string) => {
    // A stream layout change recreates every encoder, even when the toggled
    // stream is not the one currently displayed. Always give the active
    // decoder a fresh URL/generation after the rebuild.
    setPlayerKey(key => key + 1);
  }, []);

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-background md:flex-row">
      {/* Main Content Area */}
      <div className="flex w-full shrink-0 flex-col items-center justify-center pt-2 pb-1 md:h-full md:min-h-0 md:flex-1 md:shrink px-4 md:py-0">
        <div className="relative aspect-video w-full max-w-full overflow-hidden rounded-xl bg-black my-2 md:max-w-none md:rounded-2xl">
          {unsupportedPreview ? (
            <div className="flex h-full items-center justify-center gap-2 px-6 text-center text-sm text-white" role="status">
              <AlertTriangle className="h-4 w-4 shrink-0" aria-hidden="true" />
              <span>{t('sys.monitoring.hevc_preview_unsupported', '当前浏览器不支持 H.265 预览，请切换到支持 H.265 的浏览器')}</span>
            </div>
          ) : (
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
          )}
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
