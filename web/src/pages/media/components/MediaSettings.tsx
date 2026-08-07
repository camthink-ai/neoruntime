import {
  useMediaStatus,
  useSetRtspEnabled,
  useUpdateEncoderConfig,
  useReconfigureEncoder,
  useEnableStream,
  useDisableStream,
} from '../hooks/useMedia';
import { useEffect, useState, useCallback, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import MediaSettingsSkeleton from './MediaSettingsSkeleton';
import { Video, Radio, Copy } from 'lucide-react';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Card, CardContent } from '@/components/ui/card';
import { Switch } from '@/components/ui/switch';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Separator } from '@/components/ui/separator';
import { cn } from '@/lib/utils';

interface MediaSettingsProps {
  activeStream?: string;
  onStreamChange?: (streamId: string) => void;
  onStreamToggleSuccess?: (streamId: string, enabled: boolean) => void;
  onEncoderConfiguringChange?: (isConfiguring: boolean) => void;
}

export default function MediaSettings({
  activeStream: controlledActiveStream,
  onStreamChange: controlledOnStreamChange,
  onStreamToggleSuccess,
  onEncoderConfiguringChange,
}: MediaSettingsProps) {
  const { t } = useTranslation();
  const { data: statusData, isLoading, error } = useMediaStatus();

  const [activeStreamState, setActiveStreamState] = useState<string>(
    controlledActiveStream ?? 'main'
  );
  const setActiveStream = useCallback((streamId: string) => {
    setActiveStreamState(streamId);
  }, []);

  // Controlled mode: if parent provides activeStream, sync it into local state.
  useEffect(() => {
    if (!controlledActiveStream) return;
    setActiveStreamState(controlledActiveStream);
  }, [controlledActiveStream]);

  const activeStream = controlledActiveStream ?? activeStreamState;
  const onStreamChange = controlledOnStreamChange ?? setActiveStream;

  const copyText = useCallback(async (text: string) => {
    try {
      if (navigator.clipboard?.writeText) {
        await navigator.clipboard.writeText(text);
        return true;
      }
    } catch {
      // fallthrough to legacy copy
    }

    try {
      const textarea = document.createElement('textarea');
      textarea.value = text;
      textarea.setAttribute('readonly', '');
      textarea.style.position = 'fixed';
      textarea.style.left = '-9999px';
      textarea.style.top = '0';
      textarea.style.opacity = '0';
      document.body.appendChild(textarea);
      textarea.select();
      textarea.setSelectionRange(0, textarea.value.length);
      const ok = document.execCommand('copy');
      document.body.removeChild(textarea);
      return ok;
    } catch {
      return false;
    }
  }, []);

  const getStreamLabel = useCallback(
    (streamId: string) => {
      if (streamId === 'main') return t('sys.media_settings.main_stream', 'Main Stream');
      if (streamId === 'sub') return t('sys.media_settings.sub_stream', 'Sub Stream');
      if (streamId === 'third') return t('sys.media_settings.third_stream', 'Third Stream');
      return streamId;
    },
    [t]
  );

  const statusStreams = statusData?.streams ?? [];
  const STREAM_ORDER = ['main', 'sub', 'third'];
  const streamList = useMemo(() => {
    const mapped = statusStreams.map((s: any) => ({
      id: s.stream_id,
      label: getStreamLabel(s.stream_id),
      // Key enabled off has_encoder (encoder present), not status === 'active'.
      // Disabling one stream makes the backend rebuild the others, which then
      // transiently report status 'starting' (no first frame yet). Requiring
      // 'active' here flipped every other stream's Enable Stream toggle off for
      // up to staleTime (30s). has_encoder is already the "encoder running"
      // signal and is false only for genuinely removed streams.
      enabled: !!s.has_encoder,
      codec: s.codec ?? 'h264',
      width: s.width ?? 1920,
      height: s.height ?? 1080,
      fps: s.fps ?? 30,
      bitrate_bps: s.bitrate_bps ?? 4000000,
      gop: s.gop ?? 30,
    }));
    return mapped.sort((a, b) => {
      const ai = STREAM_ORDER.indexOf(a.id);
      const bi = STREAM_ORDER.indexOf(b.id);
      return (ai === -1 ? 99 : ai) - (bi === -1 ? 99 : bi);
    });
  }, [getStreamLabel, statusStreams]);

  const currentEncoder = streamList.find(s => s.id === activeStream);

  // If current active stream no longer exists, fallback to the first available one (uncontrolled mode only).
  useEffect(() => {
    if (controlledActiveStream) return;
    if (streamList.length === 0) return;
    if (streamList.some(s => s.id === activeStreamState)) return;
    setActiveStreamState(streamList[0]!.id);
  }, [activeStreamState, controlledActiveStream, streamList]);

  // RTSP enabled 状态后端不在 /media/status 返回，前端保持本地状态
  const [rtspEnabled, setRtspEnabled] = useState(true);

  // 码流编辑状态
  const [editingStream, setEditingStream] = useState<string | null>(null);
  const [streamEdits, setStreamEdits] = useState<
    Record<
      string,
      {
        codec?: string;
        width?: number;
        height?: number;
        resolutionMode?: 'preset' | 'custom';
        bitrate_bps: number;
        fps: number;
        gop: number;
      }
    >
  >({});

  // 输入草稿值：允许超出范围，失焦后再进行 clamp/对齐
  const [streamDrafts, setStreamDrafts] = useState<
    Record<
      string,
      Partial<
        Record<'width' | 'height' | 'fps' | 'bitrateKbps' | 'gop', string>
      >
    >
  >({});

  // 已保存的编辑值（热更新成功后本地保存，因为 status API 不会立即更新）
  const [savedEdits, setSavedEdits] = useState<
    Record<
      string,
      {
        codec?: string;
        width?: number;
        height?: number;
        bitrate_bps?: number;
        fps?: number;
        gop?: number;
      }
    >
  >({});

  // Hot reload mutations
  const { mutate: setRtsp } = useSetRtspEnabled();
  const { mutate: updateEncoderConfig, isPending: isUpdatingEncoder } =    useUpdateEncoderConfig();
  const { mutate: reconfigureEncoder, isPending: isReconfiguringEncoder } =    useReconfigureEncoder();

  // Either encoder PUT in flight → tell the player to show its "Configuring…"
  // overlay. PUT /api/v1/media/encoder (bitrate/fps/gop hot reload) can take a
  // while server-side; previously only the heavier reconfig (resolution/codec)
  // surfaced a loading state, so the hot-reload PUT left the player looking
  // idle mid-reconfigure.
  useEffect(() => {
    onEncoderConfiguringChange?.(isUpdatingEncoder || isReconfiguringEncoder);
  }, [
    isUpdatingEncoder,
    isReconfiguringEncoder,
    onEncoderConfiguringChange,
  ]);

  // Any encoder reconfiguration in flight (codec/resolution full reconfig, or
  // bitrate/fps/gop hot reload). While true the codec & resolution dropdowns
  // and the fps/bitrate/gop/dimension inputs stay disabled so a second change
  // can't race the in-flight request — otherwise persistStreamConfig's
  // early-return guard would silently drop an edit mid-save (UI shows the new
  // value but it never reaches the device).
  const isSavingEncoder = isUpdatingEncoder || isReconfiguringEncoder;
  const {
    mutate: enableStreamMutate,
    isPending: isEnablingStream,
    variables: enablingStreamId,
  } = useEnableStream();
  const {
    mutate: disableStreamMutate,
    isPending: isDisablingStream,
    variables: disablingStreamId,
  } = useDisableStream();

  // 常见分辨率选项（按码流区分：主码流允许更高分辨率）
  const mainResolutionOptions = [
    { value: '3840x2160', label: '3840×2160' },
    { value: '2560x1440', label: '2560×1440' },
    { value: '1920x1080', label: '1920×1080' },
    { value: '1280x720', label: '1280×720' },
    { value: '960x540', label: '960×540' },
    { value: '640x480', label: '640×480' },
    { value: '640x360', label: '640×360' },
  ];

  const otherResolutionOptions = [
    { value: '1280x720', label: '1280×720' },
    { value: '960x540', label: '960×540' },
    { value: '640x480', label: '640×480' },
    { value: '640x360', label: '640×360' },
  ];

  const getResolutionOptions = (streamId: string) => (streamId === 'main' ? mainResolutionOptions : otherResolutionOptions);

  const gcd = (a: number, b: number) => {
    let x = Math.abs(a);
    let y = Math.abs(b);
    while (y !== 0) {
      const r = x % y;
      x = y;
      y = r;
    }
    return x || 1;
  };

  const toEven = (n: number) => Math.max(2, Math.round(n / 2) * 2);

  const setStreamDraftValue = useCallback(
    (
      streamId: string,
      key: 'width' | 'height' | 'fps' | 'bitrateKbps' | 'gop',
      value: string
    ) => {
      setStreamDrafts(prev => ({
        ...prev,
        [streamId]: {
          ...(prev[streamId] ?? {}),
          [key]: value,
        },
      }));
    },
    []
  );

  const clearStreamDraftValue = useCallback(
    (
      streamId: string,
      key: 'width' | 'height' | 'fps' | 'bitrateKbps' | 'gop'
    ) => {
      setStreamDrafts(prev => {
        const next = { ...(prev[streamId] ?? {}) };
        delete next[key];
        return {
          ...prev,
          [streamId]: next,
        };
      });
    },
    []
  );

  const handleStreamInputKeyDown = useCallback(
    (event: React.KeyboardEvent<HTMLInputElement>) => {
      if (event.key !== 'Enter') return;
      event.preventDefault();
      event.currentTarget.blur();
    },
    []
  );
  const formatAspectRatio = (width?: number, height?: number) => {
    if (!width || !height) return '--';
    const d = gcd(width, height);
    return `${Math.round(width / d)}:${Math.round(height / d)}`;
  };

  const getRtspUrl = () => {
    const baseUrl = `rtsp://${window.location.hostname}:8554`;

    const normalizeStreamSuffix = (streamId: string | undefined) => {
      const raw = (streamId ?? '').trim().toLowerCase();
      if (raw === 'main' || raw === 'sub' || raw === 'third') return raw;
      if (raw.includes('third') || raw.endsWith('_third')) return 'third';
      if (raw.includes('sub') || raw.endsWith('_sub')) return 'sub';
      if (raw.includes('main') || raw.endsWith('_main')) return 'main';
      return 'main';
    };

    const streamSuffix = normalizeStreamSuffix(activeStream);

    const cleanedBase = baseUrl.replace(/\/+$/, '');
    const replaced = cleanedBase.replace(
      /\/(main|sub|third)$/i,
      `/${streamSuffix}`
    );

    return replaced.endsWith(`/${streamSuffix}`)
      ? replaced
      : `${replaced}/${streamSuffix}`;
  };

  // 保存 RTSP 配置（hot reload）
  const handleToggleRtsp = useCallback(
    (enabled: boolean) => {
      setRtspEnabled(enabled);
      setRtsp(enabled);
    },
    [setRtsp]
  );

  // Enable or disable the selected stream. Reconnect only after the API succeeds.
  const handleToggleStream = useCallback(
    (streamId: string, enabled: boolean) => {
      if (streamId === 'main') return;
      if (enabled) {
        enableStreamMutate(streamId, {
          onSuccess: () => onStreamToggleSuccess?.(streamId, true),
        });
      } else {
        disableStreamMutate(streamId, {
          onSuccess: () => onStreamToggleSuccess?.(streamId, false),
        });
      }
    },
    [disableStreamMutate, enableStreamMutate, onStreamToggleSuccess]
  );

  const getBitrateKbps = (bps: number) => Math.max(1, Math.round(bps / 1000));
  const kbpsToBps = (kbps: number) => Math.max(1000, Math.round(kbps * 1000));

  const clampNumber = (value: number, min: number, max: number) => Math.min(max, Math.max(min, value));

  // Validation limits
  const FPS_MIN = 1;
  const FPS_MAX = 30;
  const BITRATE_KBPS_MIN = 64;
  const BITRATE_KBPS_MAX_MAIN = 16384;
  const BITRATE_KBPS_MAX_SUB = 8192;
  const GOP_MIN = 1;
  const GOP_MAX = 300;

  // 开始编辑码流 — baseline 取已持久化值（savedEdits）回退到实时状态，
  // 这样之前失焦已保存的字段再次编辑时显示的是最新值，而非原始状态。
  const handleStartEdit = (streamId: string) => {
    const enc = streamList.find(s => s.id === streamId);
    if (!enc) return;
    const saved = savedEdits[streamId];
    setStreamEdits({
      [streamId]: {
        codec: saved?.codec ?? enc.codec,
        width: saved?.width ?? enc.width,
        height: saved?.height ?? enc.height,
        bitrate_bps: saved?.bitrate_bps ?? enc.bitrate_bps,
        fps: saved?.fps ?? enc.fps,
        gop: saved?.gop ?? enc.gop ?? 30,
      },
    });
    setEditingStream(streamId);
  };

  // 自动保存码流配置（无保存按钮）。overrides 携带刚改动的字段（input 失焦
  // 或 select 变更触发），其余字段从上次已持久化值（savedEdits）回退到实时状态。
  // codec / 分辨率变更走全量 reconfigure；bitrate / fps / gop 走热更新接口。
  // 保存进行中时跳过，避免并发重配置冲突。
  const persistStreamConfig = (
    streamId: string,
    overrides: {
      codec?: string;
      width?: number;
      height?: number;
      bitrate_bps?: number;
      fps?: number;
      gop?: number;
    }
  ) => {
    if (isSavingEncoder) return;

    const enc = streamList.find(s => s.id === streamId);
    if (!enc) return;
    const saved = savedEdits[streamId];

    const baseline = {
      codec: saved?.codec ?? enc.codec,
      width: saved?.width ?? enc.width,
      height: saved?.height ?? enc.height,
      bitrate_bps: saved?.bitrate_bps ?? enc.bitrate_bps,
      fps: saved?.fps ?? enc.fps,
      gop: saved?.gop ?? enc.gop ?? 30,
    };

    const effective = {
      codec: overrides.codec ?? baseline.codec,
      width: overrides.width ?? baseline.width,
      height: overrides.height ?? baseline.height,
      bitrate_bps: overrides.bitrate_bps ?? baseline.bitrate_bps,
      fps: overrides.fps ?? baseline.fps,
      gop: overrides.gop ?? baseline.gop,
    };

    // Validate FPS
    if (effective.fps < FPS_MIN || effective.fps > FPS_MAX) {
      toast.error(
        t(
          'sys.media_settings.fps_range_error',
          `FPS must be ${FPS_MIN}-${FPS_MAX}`
        )
      );
      return;
    }

    // Validate bitrate
    const bitrateKbps = getBitrateKbps(effective.bitrate_bps);
    const bitrateMax =      streamId === 'main' ? BITRATE_KBPS_MAX_MAIN : BITRATE_KBPS_MAX_SUB;
    if (bitrateKbps < BITRATE_KBPS_MIN || bitrateKbps > bitrateMax) {
      toast.error(
        t(
          'sys.media_settings.bitrate_range_error',
          `Bitrate must be ${BITRATE_KBPS_MIN}-${bitrateMax} Kbps`
        )
      );
      return;
    }

    // Validate GOP
    if (effective.gop < GOP_MIN || effective.gop > GOP_MAX) {
      toast.error(
        t(
          'sys.media_settings.gop_range_error',
          `GOP must be ${GOP_MIN}-${GOP_MAX}`
        )
      );
      return;
    }

    // 仅 codec / resolution 需要全量重配置；bitrate / framerate / gop 走热更新接口
    const needsReconfiguration =      effective.codec !== baseline.codec
      || effective.width !== baseline.width
      || effective.height !== baseline.height;

    if (needsReconfiguration) {
      reconfigureEncoder(
        {
          stream_name: streamId,
          codec: effective.codec,
          width: effective.width,
          height: effective.height,
          bitrate_bps: effective.bitrate_bps,
          fps: effective.fps,
          gop: effective.gop,
        },
        {
          onSuccess: () => {
            setSavedEdits(prev => ({
              ...prev,
              [streamId]: { ...prev[streamId], ...effective },
            }));
          },
        }
      );
    } else {
      updateEncoderConfig(
        {
          stream_name: streamId,
          bitrate_bps: effective.bitrate_bps,
          framerate: effective.fps,
          gop: effective.gop,
        },
        {
          onSuccess: () => {
            setSavedEdits(prev => ({
              ...prev,
              [streamId]: { ...prev[streamId], ...effective },
            }));
          },
        }
      );
    }
  };

  if (isLoading) {
    return (
      <ScrollArea type="auto" className="h-full">
        <MediaSettingsSkeleton />
      </ScrollArea>
    );
  }

  if (error) {
    return (
      <div className="p-6 text-sm text-red-500">
        {t('sys.media_settings.load_failed', 'Failed to load config')}:{' '}
        {(error as any)?.message}
      </div>
    );
  }

  return (
    <ScrollArea type="auto" className="h-full">
      <div className="p-4 space-y-4">
        {/* 码流设置 + 码流配置 合并 */}
        <section className="space-y-4">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-6 space-y-5">
              <h3 className="text-sm font-semibold text-muted-foreground flex items-center gap-1.5">
                <Video className="w-3.5 h-3.5" />
                {t('sys.media_settings.stream_settings', 'Stream Settings')}
              </h3>

              <div className="flex gap-2">
                {streamList.map(stream => {
                  const isSelected = activeStream === stream.id;
                  return (
                    <button
                      key={stream.id}
                      type="button"
                      onClick={() => {
                        onStreamChange(stream.id);
                        setEditingStream(null);
                        setStreamEdits({});
                      }}
                      className={cn(
                        'flex-1 rounded-md border px-3 py-2 transition-colors text-center',
                        isSelected
                          ? 'border-primary bg-primary/10 text-primary'
                          : 'border-border hover:bg-muted/30 text-foreground',
                        !stream.enabled && 'opacity-50'
                      )}
                    >
                      <div className="text-xs font-medium">{stream.label}</div>
                      {/* <div className="mt-0.5 text-[10px] text-muted-foreground font-mono">
                        {stream.width}×{stream.height}
                      </div> */}
                    </button>
                  );
                })}
              </div>

              {!currentEncoder ? (
                <div className="py-8 text-center text-sm text-muted-foreground">
                  {t(
                    'sys.media_settings.stream_not_found',
                    'Stream config not found'
                  )}
                </div>
              ) : (
                <>
                  <Separator />

                  <div className="flex items-center justify-between">
                    <div>
                      <div className="text-sm font-medium text-foreground">
                        {t(
                          'sys.media_settings.enable_stream',
                          'Enable this stream'
                        )}
                      </div>
                      <div className="text-xs text-muted-foreground mt-1">
                        {t(
                          'sys.media_settings.stream_toggle',
                          '可按需开启/关闭以节省资源'
                        )}
                      </div>
                    </div>
                    {(() => {
                      const isTogglingThisStream =                        (isEnablingStream
                          && enablingStreamId === activeStream)
                        || (isDisablingStream
                          && disablingStreamId === activeStream);

                      return (
                        <Switch
                          checked={currentEncoder.enabled}
                          disabled={
                            activeStream === 'main' || isTogglingThisStream
                          }
                          loading={isTogglingThisStream}
                          onCheckedChange={v => {
                            handleToggleStream(activeStream, v);
                          }}
                        />
                      );
                    })()}
                  </div>

                  {!currentEncoder.enabled ? (
                    <div className="py-8 text-center text-sm text-muted-foreground">
                      {t(
                        'sys.media_settings.stream_disabled_hint',
                        '码流未启用，请先启用后再配置'
                      )}
                    </div>
                  ) : (
                    <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
                      <div className="space-y-2">
                        <div className="text-xs text-muted-foreground">
                          {t('sys.media_settings.codec', 'Codec')}
                        </div>
                        <Select
                          disabled={isSavingEncoder}
                          value={String(
                            editingStream === activeStream
                              && streamEdits[activeStream]?.codec
                              ? streamEdits[activeStream]!.codec
                              : (savedEdits[activeStream]?.codec
                                  ?? currentEncoder.codec)
                          ).toLowerCase()}
                          onValueChange={value => {
                            setStreamEdits(prev => ({
                              ...prev,
                              [activeStream]: {
                                ...(prev[activeStream] ?? {
                                  codec: currentEncoder.codec,
                                  width: currentEncoder.width,
                                  height: currentEncoder.height,
                                  bitrate_bps: currentEncoder.bitrate_bps,
                                  fps: currentEncoder.fps,
                                  gop: currentEncoder.gop ?? 30,
                                }),
                                codec: value,
                              },
                            }));
                            persistStreamConfig(activeStream, { codec: value });
                            setEditingStream(activeStream);
                          }}
                        >
                          <SelectTrigger>
                            <SelectValue />
                          </SelectTrigger>
                          <SelectContent>
                            <SelectItem value="h264">H264</SelectItem>
                            <SelectItem value="h265">H265</SelectItem>
                          </SelectContent>
                        </Select>
                      </div>

                      <div className="space-y-2">
                        <div className="text-xs text-muted-foreground">
                          {t('sys.media_settings.resolution', 'Resolution')}
                        </div>
                        {(() => {
                          const edits = streamEdits[activeStream];
                          const activeWidth =                            editingStream === activeStream && edits?.width
                              ? edits.width
                              : (savedEdits[activeStream]?.width
                                ?? currentEncoder.width);
                          const activeHeight =                            editingStream === activeStream && edits?.height
                              ? edits.height
                              : (savedEdits[activeStream]?.height
                                ?? currentEncoder.height);

                          const activeResolutionValue = `${activeWidth}x${activeHeight}`;
                          const resolutionOptions =                            getResolutionOptions(activeStream);
                          const isPresetResolution = resolutionOptions.some(
                            opt => opt.value === activeResolutionValue
                          );
                          const resolutionSelectValue =                            edits?.resolutionMode === 'custom'
                              ? 'custom'
                              : isPresetResolution
                                ? activeResolutionValue
                                : 'custom';

                          return (
                            <div className="space-y-2">
                              <Select
                                disabled={isSavingEncoder}
                                value={resolutionSelectValue}
                                onValueChange={value => {
                                  if (value === 'custom') {
                                    setStreamEdits(prev => ({
                                      ...prev,
                                      [activeStream]: {
                                        ...(prev[activeStream] ?? {
                                          codec: currentEncoder.codec,
                                          width: currentEncoder.width,
                                          height: currentEncoder.height,
                                          bitrate_bps:
                                            currentEncoder.bitrate_bps,
                                          fps: currentEncoder.fps,
                                          gop: currentEncoder.gop ?? 30,
                                        }),
                                        resolutionMode: 'custom',
                                        width: activeWidth,
                                        height: activeHeight,
                                      },
                                    }));
                                    setEditingStream(activeStream);
                                    return;
                                  }

                                  const [widthRaw, heightRaw] = value
                                    .split('x')
                                    .map(Number);
                                  const width = toEven(widthRaw);
                                  const height = toEven(heightRaw);
                                  setStreamEdits(prev => ({
                                    ...prev,
                                    [activeStream]: {
                                      ...(prev[activeStream] ?? {
                                        codec: currentEncoder.codec,
                                        width: currentEncoder.width,
                                        height: currentEncoder.height,
                                        bitrate_bps: currentEncoder.bitrate_bps,
                                        fps: currentEncoder.fps,
                                        gop: currentEncoder.gop ?? 30,
                                      }),
                                      resolutionMode: 'preset',
                                      width,
                                      height,
                                    },
                                  }));
                                  persistStreamConfig(activeStream, {
                                    width,
                                    height,
                                  });
                                  setEditingStream(activeStream);
                                }}
                              >
                                <SelectTrigger>
                                  <SelectValue />
                                </SelectTrigger>
                                <SelectContent>
                                  {resolutionOptions.map(opt => (
                                    <SelectItem
                                      key={opt.value}
                                      value={opt.value}
                                    >
                                      {opt.label}
                                    </SelectItem>
                                  ))}
                                  <SelectItem value="custom">
                                    {t(
                                      'sys.media_settings.custom_resolution',
                                      '自定义'
                                    )}
                                  </SelectItem>
                                </SelectContent>
                              </Select>
                            </div>
                          );
                        })()}
                      </div>

                      {(() => {
                        const edits = streamEdits[activeStream];
                        const activeWidth =                          editingStream === activeStream && edits?.width
                            ? edits.width
                            : (savedEdits[activeStream]?.width
                              ?? currentEncoder.width);
                        const activeHeight =                          editingStream === activeStream && edits?.height
                            ? edits.height
                            : (savedEdits[activeStream]?.height
                              ?? currentEncoder.height);

                        const activeResolutionValue = `${activeWidth}x${activeHeight}`;
                        const resolutionOptions =                          getResolutionOptions(activeStream);
                        const isPresetResolution = resolutionOptions.some(
                          opt => opt.value === activeResolutionValue
                        );
                        const isCustom =                          edits?.resolutionMode === 'custom'
                          || !isPresetResolution;

                        if (!isCustom) return null;

                        return (
                          <>
                            <div className="space-y-2">
                              <div className="text-xs text-muted-foreground">
                                {t('sys.media_settings.width', 'Width')}
                              </div>
                              <Input
                                type="number"
                                min={2}
                                step={2}
                                disabled={isSavingEncoder}
                                value={
                                  streamDrafts[activeStream]?.width
                                  ?? String(activeWidth)
                                }
                                onFocus={() => {
                                  if (editingStream !== activeStream) handleStartEdit(activeStream);
                                  // 初始化草稿，避免一聚焦就被 clamp
                                  if (
                                    streamDrafts[activeStream]?.width
                                    === undefined
                                  ) {
                                    setStreamDraftValue(
                                      activeStream,
                                      'width',
                                      String(activeWidth)
                                    );
                                  }
                                }}
                                onChange={e => {
                                  setStreamDraftValue(
                                    activeStream,
                                    'width',
                                    e.target.value
                                  );
                                }}
                                onKeyDown={handleStreamInputKeyDown}
                                onBlur={() => {
                                  const raw = streamDrafts[activeStream]?.width;
                                  if (raw === undefined) return;
                                  const next = Number(raw);
                                  clearStreamDraftValue(activeStream, 'width');
                                  if (!Number.isFinite(next) || next <= 0) return;

                                  const width = toEven(next);
                                  setStreamEdits(prev => ({
                                    ...prev,
                                    [activeStream]: {
                                      ...(prev[activeStream] ?? {
                                        codec: currentEncoder.codec,
                                        width: currentEncoder.width,
                                        height: currentEncoder.height,
                                        bitrate_bps: currentEncoder.bitrate_bps,
                                        fps: currentEncoder.fps,
                                        gop: currentEncoder.gop ?? 30,
                                      }),
                                      resolutionMode: 'custom',
                                      width,
                                      height: activeHeight,
                                    },
                                  }));
                                  persistStreamConfig(activeStream, { width });
                                  setEditingStream(activeStream);
                                }}
                              />
                            </div>

                            <div className="space-y-2">
                              <div className="text-xs text-muted-foreground">
                                {t('sys.media_settings.height', 'Height')}
                              </div>
                              <Input
                                type="number"
                                min={2}
                                step={2}
                                disabled={isSavingEncoder}
                                value={
                                  streamDrafts[activeStream]?.height
                                  ?? String(activeHeight)
                                }
                                onFocus={() => {
                                  if (editingStream !== activeStream) handleStartEdit(activeStream);
                                  if (
                                    streamDrafts[activeStream]?.height
                                    === undefined
                                  ) {
                                    setStreamDraftValue(
                                      activeStream,
                                      'height',
                                      String(activeHeight)
                                    );
                                  }
                                }}
                                onChange={e => {
                                  setStreamDraftValue(
                                    activeStream,
                                    'height',
                                    e.target.value
                                  );
                                }}
                                onKeyDown={handleStreamInputKeyDown}
                                onBlur={() => {
                                  const raw =                                    streamDrafts[activeStream]?.height;
                                  if (raw === undefined) return;
                                  const next = Number(raw);
                                  clearStreamDraftValue(activeStream, 'height');
                                  if (!Number.isFinite(next) || next <= 0) return;

                                  const height = toEven(next);
                                  setStreamEdits(prev => ({
                                    ...prev,
                                    [activeStream]: {
                                      ...(prev[activeStream] ?? {
                                        codec: currentEncoder.codec,
                                        width: currentEncoder.width,
                                        height: currentEncoder.height,
                                        bitrate_bps: currentEncoder.bitrate_bps,
                                        fps: currentEncoder.fps,
                                        gop: currentEncoder.gop ?? 30,
                                      }),
                                      resolutionMode: 'custom',
                                      width: activeWidth,
                                      height,
                                    },
                                  }));
                                  persistStreamConfig(activeStream, { height });
                                  setEditingStream(activeStream);
                                }}
                              />
                            </div>

                            <div className="md:col-span-2 text-xs text-muted-foreground">
                              {t(
                                'sys.media_settings.aspect_ratio',
                                'Aspect Ratio'
                              )}
                              :{' '}
                              <span className="font-mono text-foreground/90">
                                {formatAspectRatio(activeWidth, activeHeight)}
                              </span>
                            </div>
                          </>
                        );
                      })()}

                      <div className="space-y-2">
                        <div className="text-xs text-muted-foreground">
                          {t('sys.media_settings.fps', 'Frame Rate')} (FPS)
                        </div>
                        <Input
                          type="number"
                          min={FPS_MIN}
                          max={FPS_MAX}
                          disabled={isSavingEncoder}
                          value={
                            streamDrafts[activeStream]?.fps
                            ?? String(
                              editingStream === activeStream
                                && streamEdits[activeStream]?.fps !== undefined
                                ? streamEdits[activeStream]!.fps
                                : (savedEdits[activeStream]?.fps
                                    ?? currentEncoder.fps)
                            )
                          }
                          onFocus={() => {
                            if (editingStream !== activeStream) handleStartEdit(activeStream);
                            if (streamDrafts[activeStream]?.fps === undefined) {
                              const currentValue =                                editingStream === activeStream
                                && streamEdits[activeStream]?.fps !== undefined
                                  ? streamEdits[activeStream]!.fps
                                  : (savedEdits[activeStream]?.fps
                                    ?? currentEncoder.fps);
                              setStreamDraftValue(
                                activeStream,
                                'fps',
                                String(currentValue)
                              );
                            }
                          }}
                          onChange={e => {
                            setStreamDraftValue(
                              activeStream,
                              'fps',
                              e.target.value
                            );
                          }}
                          onKeyDown={handleStreamInputKeyDown}
                          onBlur={() => {
                            const raw = streamDrafts[activeStream]?.fps;
                            if (raw === undefined) return;
                            const next = Number(raw);
                            clearStreamDraftValue(activeStream, 'fps');
                            if (Number.isNaN(next)) return;

                            const fps = clampNumber(next, FPS_MIN, FPS_MAX);
                            setStreamEdits(prev => ({
                              ...prev,
                              [activeStream]: {
                                ...(prev[activeStream] ?? {
                                  codec: currentEncoder.codec,
                                  width: currentEncoder.width,
                                  height: currentEncoder.height,
                                  bitrate_bps: currentEncoder.bitrate_bps,
                                  fps: currentEncoder.fps,
                                  gop: currentEncoder.gop ?? 30,
                                }),
                                fps,
                              },
                            }));
                            persistStreamConfig(activeStream, { fps });
                            setEditingStream(activeStream);
                          }}
                        />
                      </div>

                      <div className="space-y-2">
                        <div className="text-xs text-muted-foreground">
                          {t('sys.media_settings.bitrate', 'Bitrate')} (Kbps)
                        </div>
                        <Input
                          type="number"
                          min={BITRATE_KBPS_MIN}
                          max={
                            activeStream === 'main'
                              ? BITRATE_KBPS_MAX_MAIN
                              : BITRATE_KBPS_MAX_SUB
                          }
                          disabled={isSavingEncoder}
                          value={
                            streamDrafts[activeStream]?.bitrateKbps
                            ?? String(
                              editingStream === activeStream
                                && streamEdits[activeStream]?.bitrate_bps
                                  !== undefined
                                ? getBitrateKbps(
                                    streamEdits[activeStream]!.bitrate_bps
                                  )
                                : getBitrateKbps(
                                    savedEdits[activeStream]?.bitrate_bps
                                      ?? currentEncoder.bitrate_bps
                                  )
                            )
                          }
                          onFocus={() => {
                            if (editingStream !== activeStream) handleStartEdit(activeStream);
                            if (
                              streamDrafts[activeStream]?.bitrateKbps
                              === undefined
                            ) {
                              const currentValue =                                editingStream === activeStream
                                && streamEdits[activeStream]?.bitrate_bps
                                  !== undefined
                                  ? getBitrateKbps(
                                      streamEdits[activeStream]!.bitrate_bps
                                    )
                                  : getBitrateKbps(
                                      savedEdits[activeStream]?.bitrate_bps
                                        ?? currentEncoder.bitrate_bps
                                    );
                              setStreamDraftValue(
                                activeStream,
                                'bitrateKbps',
                                String(currentValue)
                              );
                            }
                          }}
                          onChange={e => {
                            setStreamDraftValue(
                              activeStream,
                              'bitrateKbps',
                              e.target.value
                            );
                          }}
                          onKeyDown={handleStreamInputKeyDown}
                          onBlur={() => {
                            const raw = streamDrafts[activeStream]?.bitrateKbps;
                            if (raw === undefined) return;
                            const next = Number(raw);
                            clearStreamDraftValue(activeStream, 'bitrateKbps');
                            if (Number.isNaN(next)) return;

                            const kbps = clampNumber(
                              next,
                              BITRATE_KBPS_MIN,
                              activeStream === 'main'
                                ? BITRATE_KBPS_MAX_MAIN
                                : BITRATE_KBPS_MAX_SUB
                            );
                            setStreamEdits(prev => ({
                              ...prev,
                              [activeStream]: {
                                ...(prev[activeStream] ?? {
                                  codec: currentEncoder.codec,
                                  width: currentEncoder.width,
                                  height: currentEncoder.height,
                                  bitrate_bps: currentEncoder.bitrate_bps,
                                  fps: currentEncoder.fps,
                                  gop: currentEncoder.gop ?? 30,
                                }),
                                bitrate_bps: kbpsToBps(kbps),
                              },
                            }));
                            persistStreamConfig(activeStream, {
                              bitrate_bps: kbpsToBps(kbps),
                            });
                            setEditingStream(activeStream);
                          }}
                        />
                      </div>

                      <div className="space-y-2">
                        <div className="text-xs text-muted-foreground">
                          {t('sys.media_settings.gop', 'I帧间隔 (GOP)')}
                        </div>
                        <Input
                          type="number"
                          className="w-full md:max-w-[220px]"
                          min={GOP_MIN}
                          max={GOP_MAX}
                          disabled={isSavingEncoder}
                          value={
                            streamDrafts[activeStream]?.gop
                            ?? String(
                              editingStream === activeStream
                                && streamEdits[activeStream]?.gop !== undefined
                                ? streamEdits[activeStream]!.gop
                                : (savedEdits[activeStream]?.gop
                                    ?? currentEncoder.gop
                                    ?? 30)
                            )
                          }
                          onFocus={() => {
                            if (editingStream !== activeStream) handleStartEdit(activeStream);
                            if (streamDrafts[activeStream]?.gop === undefined) {
                              const currentValue =                                editingStream === activeStream
                                && streamEdits[activeStream]?.gop !== undefined
                                  ? streamEdits[activeStream]!.gop
                                  : (savedEdits[activeStream]?.gop
                                    ?? currentEncoder.gop
                                    ?? 30);
                              setStreamDraftValue(
                                activeStream,
                                'gop',
                                String(currentValue)
                              );
                            }
                          }}
                          onChange={e => {
                            setStreamDraftValue(
                              activeStream,
                              'gop',
                              e.target.value
                            );
                          }}
                          onKeyDown={handleStreamInputKeyDown}
                          onBlur={() => {
                            const raw = streamDrafts[activeStream]?.gop;
                            if (raw === undefined) return;
                            const next = Number(raw);
                            clearStreamDraftValue(activeStream, 'gop');
                            if (Number.isNaN(next)) return;

                            const gop = clampNumber(next, GOP_MIN, GOP_MAX);
                            setStreamEdits(prev => ({
                              ...prev,
                              [activeStream]: {
                                ...(prev[activeStream] ?? {
                                  codec: currentEncoder.codec,
                                  width: currentEncoder.width,
                                  height: currentEncoder.height,
                                  bitrate_bps: currentEncoder.bitrate_bps,
                                  fps: currentEncoder.fps,
                                  gop: currentEncoder.gop ?? 30,
                                }),
                                gop,
                              },
                            }));
                            persistStreamConfig(activeStream, { gop });
                            setEditingStream(activeStream);
                          }}
                        />
                      </div>
                    </div>
                  )}
                </>
              )}
            </CardContent>
          </Card>
        </section>

        {/* RTSP 服务 */}
        <section className="space-y-4">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-5 space-y-3">
              <h3 className="text-sm font-semibold text-muted-foreground flex items-center gap-1.5">
                <Radio className="w-3.5 h-3.5" />
                {t('sys.media_settings.rtsp', 'RTSP 服务')}
              </h3>

              <div className="flex items-center justify-between gap-3">
                <div className="text-xs text-muted-foreground">
                  {t('sys.media_settings.rtsp_enabled', 'Enable RTSP Stream')}
                </div>
                <Switch
                  checked={rtspEnabled}
                  onCheckedChange={handleToggleRtsp}
                />
              </div>

              <p className="text-xs text-muted-foreground">
                {t(
                  'sys.media_settings.rtsp_hint',
                  'Allow access via standard protocol'
                )}
              </p>

              <div className="space-y-2">
                <div className="text-xs text-muted-foreground">
                  {t(
                    'sys.media_settings.rtsp_url',
                    'RTSP URL for selected stream'
                  )}
                  :
                </div>
                <div className="flex items-center gap-2">
                  <Input value={getRtspUrl()} readOnly className="font-mono" />
                  <Button
                    type="button"
                    variant="outline"
                    onClick={async () => {
                      const ok = await copyText(getRtspUrl());
                      if (ok) {
                        return;
                      }
                      toast.error(t('common.copy_failed', 'Copy failed'));
                    }}
                    aria-label={t('common.copy', 'Copy')}
                  >
                    <Copy className="w-4 h-4" />
                  </Button>
                </div>
              </div>
            </CardContent>
          </Card>
        </section>
      </div>
    </ScrollArea>
  );
}
