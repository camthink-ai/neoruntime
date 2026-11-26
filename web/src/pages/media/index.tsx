import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Player from '@/components/player/player';
import {
  SoftwareHevcNotice,
  useSoftwareHevcNotice,
} from '@/components/player/software-hevc-notice';
import MediaSettings from './components/MediaSettings';
import { getItem } from '@/utils/storage';
import { useMediaStatus } from './hooks/useMedia';
import { useStreamFallback } from '@/hooks/useStreamFallback';

export default function Monitoring() {
  const { t } = useTranslation();
  const playerRef = useRef<VideoStreamPlayer | null>(null);
  const softwareNotice = useSoftwareHevcNotice();

  const [activeStream, setActiveStream] = useState<'main' | 'sub' | 'third'>(
    'main'
  );
  const [isEncoderConfiguring, setIsEncoderConfiguring] = useState(false);

  const { data: mediaStatus } = useMediaStatus();

  const streams = mediaStatus?.streams ?? [];

  const { effectiveStreamId, effectiveStreamInfo, codecNotice } = useStreamFallback({
    streams,
    activeStream,
  });

  // Surface terminal codec states: switched to H.264 (info) or no playable
  // stream at all (error). Without this the player sits disconnected with
  // no explanation of why HEVC playback is impossible.
  useEffect(() => {
    if (!codecNotice) return;
    if (codecNotice === 'hevc_unsupported_fallback') toast.info(t(`sys.monitoring.${codecNotice}`));
    else toast.error(t(`sys.monitoring.${codecNotice}`));
  }, [codecNotice, t]);

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
  const currentStreamUrl = effectiveEnabled
    ? `${buildStreamUrl(effectiveStreamId)}&reconnect=${playerKey}`
    : '';

  const handleStreamToggleSuccess = useCallback(
    (streamId: string) => {
      if (streamId !== effectiveStreamId) return;
      setPlayerKey(key => key + 1);
    },
    [effectiveStreamId]
  );

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-background md:flex-row">
      {/* Main Content Area */}
      <div className="flex w-full shrink-0 flex-col items-center justify-center pt-2 pb-1 md:h-full md:min-h-0 md:flex-1 md:shrink px-4 md:py-0">
        {softwareNotice.visible && (
          <SoftwareHevcNotice onDismiss={softwareNotice.dismiss} />
        )}
        <div className="relative aspect-video w-full max-w-full overflow-hidden rounded-xl bg-black my-2 md:max-w-none md:rounded-2xl">
          <Player
            key={`${effectiveStreamId}:${playerKey}`}
            videoUrl={currentStreamUrl}
            videoRendererInstance={playerRef}
            streamWidth={activeStreamInfo?.width}
            streamHeight={activeStreamInfo?.height}
            objectFit="adaptive"
            audioOverlay
            configuring={isEncoderConfiguring}
            onSoftwareHevcChange={softwareNotice.onChange}
          />
        </div>
      </div>

      {/* Config sidebar — full-width at the bottom with a top border on mobile, fixed width on the right on desktop */}
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
          />
        </div>
      </div>
    </div>
  );
}
