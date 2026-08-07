import { useTranslation } from 'react-i18next';
import {
  VideoOff,
  AlertTriangle,
  ExternalLink,
  MonitorPlay,
} from 'lucide-react';
import { useNavigate } from 'react-router-dom';
import { useMediaStatus } from '@/hooks/useMediaStatus';
import { useStreamFallback } from '@/hooks/useStreamFallback';
import { getItem } from '@/utils/storage';
import { useState, useRef, useEffect, useCallback } from 'react';
import Player from '@/components/player/player';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Loading from '@/components/loading';
import { Button } from '@/components/ui/button';

type StreamStatus =
  | 'loading'
  | 'playing'
  | 'no_camera'
  | 'error'
  | 'disconnected';

export default function StreamPreview() {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const playerRef = useRef<VideoStreamPlayer | null>(null);
  const codecFallbackPendingRef = useRef(false);
  const [status, setStatus] = useState<StreamStatus>('loading');
  const [videoUrl, setVideoUrl] = useState('');

  const { data: mediaStatus, isLoading, error } = useMediaStatus();
  const streams = mediaStatus?.streams ?? [];
  const { effectiveStreamId, codecNotice } = useStreamFallback({
    streams,
    activeStream: 'sub',
  });

  const getStreamUrl = useCallback((streamId: string) => {
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }

    const baseUrl = window.location.origin.replace(/^http/, 'ws');
    return `${baseUrl}/api/v1/h264/${streamId}?token=${encodeURIComponent(token)}`;
  }, []);

  // Auto-play when streams are available
  useEffect(() => {
    if (isLoading) return;

    // /api/v1/streams 目前可能不可用（404），但播放可以走 h264 默认地址
    if (!error) {
      setVideoUrl(getStreamUrl(effectiveStreamId));
      setStatus('loading');
      return;
    }

    setVideoUrl('');
    setStatus('no_camera');
  }, [isLoading, error, getStreamUrl, effectiveStreamId]);

  // Listen for player events to detect stream errors
  useEffect(() => {
    if (!videoUrl) return;

    const handleWork = (e: Event) => {
      const playing = (e as CustomEvent<boolean>).detail;
      if (playing) {
        codecFallbackPendingRef.current = false;
        setStatus('playing');
      }
    };

    const handleClose = (e: Event) => {
      if (codecFallbackPendingRef.current) return;
      const evt = e as CustomEvent<{ code?: number; reason?: string }>;
      const reason = evt.detail?.reason;
      if (reason === 'Connection replaced') {
        setStatus('disconnected');
      } else {
        setStatus('error');
      }
    };

    const handleError = (e: Event) => {
      const { detail } = e as CustomEvent<{ type?: string }>;
      if (
        detail?.type === 'codec_mismatch'
        && streams.some(stream => stream.codec.toLowerCase() === 'h264')
      ) {
        codecFallbackPendingRef.current = true;
        setStatus('loading');
        return;
      }
      setStatus('error');
    };

    window.addEventListener('wv_work', handleWork);
    window.addEventListener('wv_close', handleClose);
    window.addEventListener('wv_error', handleError);
    return () => {
      window.removeEventListener('wv_work', handleWork);
      window.removeEventListener('wv_close', handleClose);
      window.removeEventListener('wv_error', handleError);
    };
  }, [videoUrl, streams]);

  const getStatusContent = () => {
    switch (status) {
      case 'no_camera':
        return (
          <div className="absolute inset-0 flex flex-col items-center justify-center gap-2 text-muted-foreground">
            <VideoOff className="w-10 h-10" />
            <span className="text-xs">{t('sys.dashboard.no_camera')}</span>
          </div>
        );
      case 'error':
        return (
          <div className="absolute inset-0 flex flex-col items-center justify-center gap-2 text-muted-foreground">
            <AlertTriangle className="w-10 h-10" />
            <span className="text-xs">{t('sys.dashboard.stream_error')}</span>
          </div>
        );
      case 'disconnected':
        return (
          <div className="absolute inset-0 flex flex-col items-center justify-center gap-2 text-muted-foreground">
            <VideoOff className="w-10 h-10" />
            <span className="text-xs">
              {t('sys.dashboard.stream_disconnected')}
            </span>
          </div>
        );
      default:
        return null;
    }
  };

  return (
    <div className="bg-card rounded-2xl p-4 shadow-sm border border-border flex flex-col h-full min-h-[260px] lg:min-h-0 overflow-hidden">
      <div className="flex items-center justify-between mb-3 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <MonitorPlay className="w-4 h-4 text-primary" />
          {t('sys.dashboard.stream_preview', '媒体预览')}
        </h3>
        <Button
          variant="ghost"
          size="icon"
          onClick={() => navigate('/media')}
          title={t('sys.dashboard.go_media')}
        >
          <ExternalLink className="w-4 h-4 text-muted-foreground" />
        </Button>
      </div>

      <div className="flex-1 min-h-0 min-w-0 relative bg-secondary/20 rounded-lg overflow-hidden">
        {codecNotice === 'hevc_unsupported_fallback' && (
          <div className="absolute left-2 right-2 top-2 z-10 rounded bg-black/60 px-2 py-1 text-[11px] text-white/80">
            {t(
              'sys.monitoring.hevc_unsupported_fallback',
              'H.265 is unsupported; switched to H.264 playback'
            )}
          </div>
        )}
        {isLoading ? (
          <div className="absolute inset-0 flex items-center justify-center">
            <Loading fullHeight={false} size="sm" />
          </div>
        ) : videoUrl && (status === 'loading' || status === 'playing') ? (
          <div
            className="absolute inset-0 h-full w-full"
            onDoubleClick={e => e.preventDefault()}
          >
            <Player
              videoUrl={videoUrl}
              videoRendererInstance={playerRef}
              showPanel={false}
              enableDoubleClickFullscreen={false}
              enableAudio={false}
              objectFit="cover"
            />
          </div>
        ) : (
          getStatusContent()
        )}
      </div>
    </div>
  );
}
