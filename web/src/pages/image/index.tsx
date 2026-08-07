// @refresh reset
import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { VideoStreamPlayer } from '@/lib/videoStream/player';
import Player from '@/components/player/player';
import { getItem } from '@/utils/storage';
import ImagingControls from './components/ImagingControls';
import ImageSettings from './components/ImageSettings';
import OsdSettings from './components/OsdSettings';
import PrivacyMaskSettings from './components/PrivacyMaskSettings';
import OsdOverlayLayer from './components/OsdOverlayLayer';
import PrivacyMaskOverlayLayer from './components/PrivacyMaskOverlayLayer';
import { useOsdConfig, type StreamName } from './hooks/useOsdConfig';
import { usePrivacyMaskConfig } from './hooks/usePrivacyMaskConfig';
import { useMediaStatus } from '@/hooks/useMediaStatus';
import { useStreamFallback } from '@/hooks/useStreamFallback';
import { useCapabilities } from '@/hooks/useDeviceControl';

type TabKey = 'imaging' | 'image' | 'overlay';

export default function Image() {
  const { t } = useTranslation();
  const [activeTab, setActiveTab] = useState<TabKey>('image');
  const [activeStream, setActiveStream] = useState<StreamName>('main');

  const { data: caps } = useCapabilities();
  const { data: mediaStatus } = useMediaStatus();

  const capsRecord = caps as Record<string, boolean> | undefined;
  const hasLed = !caps || capsRecord?.has_led;
  const hasMcu = !caps || capsRecord?.has_mcu;

  const streams = mediaStatus?.streams ?? [];

  // Fetch overlay state only while the overlay tab is active (mirrors the
  // media page's gating so the OSD/privacy APIs aren't hit off-tab).
  const osd = useOsdConfig(activeStream, activeTab === 'overlay');
  const privacy = usePrivacyMaskConfig(activeTab === 'overlay');

  // ---- Persistent live player (mounted ONCE, survives all tab switches) ----
  // Previously each tab owned its own <Player> (ImagingPreview vs OverlayPreview),
  // so switching tabs unmounted+remounted the player and re-initialized the
  // websocket/decoder. Lifted here so only the sidebar + overlay layers swap.
  const playerRef = useRef<VideoStreamPlayer | null>(null);
  const [playbackFrameSize, setPlaybackFrameSize] = useState<{
    width: number;
    height: number;
  } | null>(null);
  const { effectiveStreamId, effectiveStreamInfo, codecNotice } =    useStreamFallback({
      streams,
      activeStream,
    });
  const frameWidth = playbackFrameSize?.width ?? effectiveStreamInfo?.width;
  const frameHeight = playbackFrameSize?.height ?? effectiveStreamInfo?.height;

  useEffect(() => {
    setPlaybackFrameSize(null);
  }, [effectiveStreamId]);

  const handleFrameSizeChange = useCallback((size: {
    width: number;
    height: number;
  }) => {
    setPlaybackFrameSize(prev => (
      prev?.width === size.width && prev?.height === size.height ? prev : size
    ));
  }, []);

  const getCurrentStreamUrl = () => {
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    const baseUrl = window.location.origin.replace(/^http/, 'ws');
    return `${baseUrl}/api/v1/h264/${effectiveStreamId}?token=${encodeURIComponent(token)}`;
  };

  const [currentStreamUrl, setCurrentStreamUrl] = useState<string>(
    getCurrentStreamUrl()
  );

  useEffect(() => {
    setCurrentStreamUrl(getCurrentStreamUrl());
  }, [effectiveStreamId]);

  const noticeText = codecNotice
    ? t(
        `sys.monitoring.${codecNotice}`,
        codecNotice === 'hevc_unsupported_fallback'
          ? '当前浏览器不支持 H.265 解码，已切换为 H.264 播放'
          : '当前浏览器不支持 H.265 解码，且无可用 H.264 码流'
      )
    : null;

  const handleStreamChange = (streamId: string) => {
    if (streamId === 'main' || streamId === 'sub' || streamId === 'third') {
      setActiveStream(streamId as StreamName);
    }
  };

  const tabs: { id: TabKey; label: string }[] = [
    { id: 'image', label: t('sys.device.image.title', 'Image') },
    { id: 'overlay', label: t('sys.device.overlay.title', 'Overlay') },
    { id: 'imaging', label: t('sys.device.imaging.title', 'Control') },
  ];

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-background md:flex-row">
      {/* Main Content Area — player persists across tab switches */}
      <div className="flex w-full shrink-0 flex-col items-center justify-center px-3 pt-2 pb-1 md:h-full md:min-h-0 md:flex-1 md:shrink md:px-4 md:pb-4">
        {noticeText && (
          <div className="w-full pb-1 text-[11px] leading-tight text-muted-foreground/70">
            {noticeText}
          </div>
        )}
        <div
          className="relative aspect-video w-full max-w-full overflow-hidden rounded-xl bg-black md:max-w-none md:rounded-2xl"
          onPointerDown={() => {
            // 点击播放区空白处（非 OSD overlay 框/手柄、非隐私遮挡多边形/顶点）
            // 取消选中：上述元素的 pointerdown 均 stopPropagation，不会触发此处，
            // 故此处仅捕获空白点击 → player 内文字框/遮挡区域回到完成态。
            osd.setSelectedId(null);
            privacy.setActiveRegion(null);
          }}
        >
          <Player
            videoUrl={currentStreamUrl}
            videoRendererInstance={playerRef}
            streamWidth={frameWidth}
            streamHeight={frameHeight}
            onFrameSizeChange={handleFrameSizeChange}
            showPanel={false}
            enableDoubleClickFullscreen={false}
            enableAudio={false}
            objectFit="adaptive"
          />
          {/* Overlay interaction layers — only on the overlay tab, drawn over
              the same persistent player (WYSIWYG). Off-tab the stream is clean. */}
          {activeTab === 'overlay' && (
            <>
              <PrivacyMaskOverlayLayer
                {...privacy}
                streamWidth={frameWidth}
                streamHeight={frameHeight}
              />
              <OsdOverlayLayer
                activeStream={activeStream}
                textOverlays={osd.textOverlays}
                datetimeOverlays={osd.datetimeOverlays}
                imageOverlays={osd.imageOverlays}
                streamWidth={frameWidth}
                streamHeight={frameHeight}
                selectedId={osd.selectedId}
                onSelect={osd.setSelectedId}
                onUpdateText={osd.updateTextOverlay}
                onUpdateDateTime={osd.updateDateTimeOverlay}
                onUpdateImage={osd.updateImageOverlay}
              />
            </>
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

        {/* Tabs Navigation */}
        <div className="flex border-b border-border px-6 pt-3 gap-6 overflow-x-auto">
          {tabs.map(tab => (
            <button
              key={tab.id}
              type="button"
              onClick={() => setActiveTab(tab.id)}
              className={`shrink-0 pb-2 text-sm font-medium transition-colors border-b-2 ${
                activeTab === tab.id
                  ? 'border-primary text-primary'
                  : 'border-transparent text-muted-foreground hover:text-foreground'
              }`}
            >
              {tab.label}
            </button>
          ))}
        </div>

        {/* Tab Content */}
        <div className="relative min-h-0 flex-1 overflow-y-auto p-4">
          {activeTab === 'imaging' && (
            <ImagingControls hasMcu={hasMcu} hasLed={hasLed} />
          )}
          {activeTab === 'image' && <ImageSettings />}
          {activeTab === 'overlay' && (
            <div className="space-y-4">
              <OsdSettings
                activeStream={activeStream}
                onStreamChange={handleStreamChange}
                loading={osd.loading}
                saveStatus={osd.saveStatus}
                selectedId={osd.selectedId}
                onSelect={osd.setSelectedId}
                textOverlays={osd.textOverlays}
                datetimeOverlays={osd.datetimeOverlays}
                imageOverlays={osd.imageOverlays}
                addTextOverlay={osd.addTextOverlay}
                updateTextOverlay={osd.updateTextOverlay}
                removeTextOverlay={osd.removeTextOverlay}
                addDateTimeOverlay={osd.addDateTimeOverlay}
                updateDateTimeOverlay={osd.updateDateTimeOverlay}
                addImageOverlay={osd.addImageOverlay}
                updateImageOverlay={osd.updateImageOverlay}
                removeImageOverlay={osd.removeImageOverlay}
              />
              <PrivacyMaskSettings {...privacy} />
            </div>
          )}
        </div>
      </div>
    </div>
  );
}
