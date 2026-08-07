import { useEffect, useRef, useState } from 'react';
import { ExternalLink, MonitorPlay } from 'lucide-react';
import { Button } from '@/components/ui/button';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { useTranslation } from 'react-i18next';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Player from '@/components/player/player';
import { useStreams } from '@/hooks/useStreams';
import { getItem } from '@/utils/storage';
import { Link } from 'react-router-dom';

export default function VideoPreview() {
  const { t } = useTranslation();
  const playerRef = useRef<VideoStreamPlayer | null>(null);
  const { data: streams } = useStreams();
  const [selectedStreamId, setSelectedStreamId] = useState<string>('main');

  // Get stream URL by stream ID
  const getStreamUrl = (streamId: string) => {
    // Get token for WebSocket authentication
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }

    const baseUrl = window.location.origin.replace(/^http/, 'ws');
    return `${baseUrl}/api/v1/h264/${streamId}?token=${encodeURIComponent(token)}`;
  };

  const streamList = streams || [];
  const selectedStream = streamList.find(s => s.stream_id === selectedStreamId);
  const mainStream = streamList.find(s => s.stream_id === 'main');
  const fallbackToMain =    selectedStreamId !== 'main'
    && selectedStream?.status !== 'active'
    && mainStream?.status === 'active';
  const effectiveStreamId = fallbackToMain ? 'main' : selectedStreamId;
  const videoUrl = getStreamUrl(effectiveStreamId);
  const currentStream = streamList.find(s => s.stream_id === effectiveStreamId);

  useEffect(() => {
    if (fallbackToMain) {
      setSelectedStreamId('main');
    }
  }, [fallbackToMain]);

  const subStreamEnabled =    streamList.find(s => s.stream_id === 'sub')?.status === 'active';
  const thirdStreamEnabled =    streamList.find(s => s.stream_id === 'third')?.status === 'active';

  return (
    <div className="bg-card rounded-2xl shadow-sm border border-border overflow-hidden flex flex-col h-full">
      {/* Header */}
      <div className="p-4 flex items-center justify-between border-b border-border/50">
        <div className="flex items-center">
          <MonitorPlay className="w-5 h-5 text-muted-foreground mr-2" />
          <Select value={selectedStreamId} onValueChange={setSelectedStreamId}>
            <SelectTrigger className="w-[140px]">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              <SelectItem value="main">
                {t('sys.media_settings.main_stream', '主码流')}
              </SelectItem>
              <SelectItem value="sub" disabled={!subStreamEnabled}>
                {t('sys.media_settings.sub_stream', '子码流')}
              </SelectItem>
              <SelectItem value="third" disabled={!thirdStreamEnabled}>
                {t('sys.media_settings.third_stream', '第三码流')}
              </SelectItem>
            </SelectContent>
          </Select>
        </div>
        <div className="flex items-center space-x-3">
          <div className="bg-secondary/50 px-3 py-1.5 rounded-md text-xs font-bold text-foreground text-center">
            {currentStream?.width && currentStream?.height
              ? `${currentStream.width}x${currentStream.height}`
              : '1920x1080'}
          </div>
          <Link to="/media">
            <Button
              variant="ghost"
              size="icon"
              className="h-9 w-9 bg-secondary/30 text-muted-foreground hover:text-foreground hover:bg-secondary/50"
            >
              <ExternalLink className="w-4 h-4" />
            </Button>
          </Link>
        </div>
      </div>

      {/* Video Area - 16:9 aspect ratio */}
      <div className="flex-1  relative">
        <div className="aspect-video w-full h-full flex items-center justify-center">
          <Player
            videoUrl={videoUrl}
            videoRendererInstance={playerRef}
            enableDoubleClickFullscreen={false}
            enableAudio={false}
          />
        </div>
      </div>
    </div>
  );
}
