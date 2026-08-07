import { useEffect, useRef, useState, type FocusEvent } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Label } from '@/components/ui/label';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { Switch } from '@/components/ui/switch';
import { Card, CardContent } from '@/components/ui/card';
import { ScrollArea } from '@/components/ui/scroll-area';
import {
  Video,
  TypeIcon,
  ClockIcon,
  ImageIcon,
  Eye,
  EyeOff,
  Trash2,
  Plus,
  Upload,
} from 'lucide-react';
import { cn } from '@/lib/utils';
import {
  uploadOsdImage,
  type OsdTextOverlay,
  type OsdDateTimeOverlay,
  type OsdImageOverlay,
} from '@/services/media';
import { CORNERS } from './OsdOverlayLayer';
import { OsdSettingsSkeleton } from './OverlaySkeletons';
import SaveStatusIndicator from './SaveStatusIndicator';
import type { SaveStatus } from '../hooks/useSaveStatus';
import { MAX_IMAGE_OVERLAYS, type StreamName } from '../hooks/useOsdConfig';

interface OsdSettingsProps {
  /** Stream currently playing in the live Player — editing tracks what you see. */
  activeStream: StreamName;
  onStreamChange: (s: StreamName) => void;
  loading: boolean;
  saveStatus: SaveStatus;
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  textOverlays: OsdTextOverlay[];
  datetimeOverlays: OsdDateTimeOverlay[];
  imageOverlays: OsdImageOverlay[];
  addTextOverlay: () => void;
  updateTextOverlay: (id: string, changes: Partial<OsdTextOverlay>) => void;
  removeTextOverlay: (id: string) => void;
  addDateTimeOverlay: () => void;
  updateDateTimeOverlay: (
    id: string,
    changes: Partial<OsdDateTimeOverlay>
  ) => void;
  addImageOverlay: () => void;
  updateImageOverlay: (id: string, changes: Partial<OsdImageOverlay>) => void;
  removeImageOverlay: (id: string) => void;
}

// Range mirrors the on-video drag-resize clamp in OsdOverlayLayer so the
// slider and the corner handle stay in sync (8..72 px @ stream resolution).
const FONT_SIZE_MIN = 8;
const FONT_SIZE_MAX = 72;

// Image overlay upload limits. The per-file cap rejects oversized uploads
// client-side (the device OSD path is PNG/BMP only and the daemon upload is
// not designed for multi-megabyte bitmaps); the count cap (MAX_IMAGE_OVERLAYS,
// imported from useOsdConfig) keeps the overlay pipeline bounded — each image
// is a separate overlay slot on the stream.
const MAX_IMAGE_SIZE_BYTES = 5 * 1024 * 1024; // 5 MB

const formatImageFileSize = (bytes?: number): string => {
  if (!bytes || bytes < 0) return '';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
};

function FontSizeRow({
  value,
  onChange,
  onDone,
}: {
  value: number;
  onChange: (v: number) => void;
  /** 失焦（焦点移出当前 overlay 项）时触发，用于让 player 退出编辑态。 */
  onDone?: () => void;
}) {
  const { t } = useTranslation();
  const rounded = Math.round(value);
  const [draft, setDraft] = useState<string>(String(rounded));
  const rootRef = useRef<HTMLDivElement>(null);

  // Keep the draft in sync when the value changes externally (e.g. the on-video
  // drag-resize handle changes the font size while typing isn't in progress).
  useEffect(() => {
    setDraft(String(rounded));
  }, [rounded]);

  // Clamp + commit on blur so the user can freely type multi-digit values
  // (e.g. "50") without each keystroke snapping into the [8, 72] range.
  const commit = () => {
    const n = Number(draft);
    if (Number.isNaN(n)) {
      setDraft(String(rounded));
      return;
    }
    const clamped = Math.min(
      FONT_SIZE_MAX,
      Math.max(FONT_SIZE_MIN, Math.round(n))
    );
    setDraft(String(clamped));
    if (clamped !== rounded) onChange(clamped);
  };

  // 失焦提交，并在焦点移出当前 overlay 项时退出选中态（player 编辑态→完成态）。
  // 仅当焦点确实离开本项才取消选中，避免在项内切换控件（如文本输入）时误清除。
  const handleBlur = (e: FocusEvent<HTMLInputElement>) => {
    commit();
    const next = e.relatedTarget as Node | null;
    const item = rootRef.current?.parentElement;
    if (!next || !item || !item.contains(next)) {
      onDone?.();
    }
  };

  return (
    <div ref={rootRef} className="flex items-center gap-2">
      <Label className="text-[10px] text-muted-foreground shrink-0">
        {t('sys.media_settings.osd_font_size', 'Font Size')}
      </Label>
      <Input
        type="number"
        min={FONT_SIZE_MIN}
        max={FONT_SIZE_MAX}
        value={draft}
        onChange={e => setDraft(e.target.value)}
        onBlur={handleBlur}
        className="h-7 w-16 text-xs"
      />
    </div>
  );
}

/**
 * Presentational OSD editor. All overlay state + handlers are owned by the
 * common parent (`media/index.tsx`) via `useOsdConfig` and passed down, so the
 * live-video interaction layer (`OsdOverlayLayer`, rendered in the parent) and
 * this sidebar form stay in sync without a portal. This component only renders
 * the sidebar fine-tune controls (text content, X/Y/font, datetime corner +
 * format, image upload + X/Y/W/H) — the primary edit path is drag/resize/type
 * directly on the video.
 */
export default function OsdSettings({
  activeStream,
  onStreamChange,
  loading,
  selectedId,
  onSelect,
  saveStatus,
  textOverlays,
  datetimeOverlays,
  imageOverlays,
  addTextOverlay,
  updateTextOverlay,
  removeTextOverlay,
  addDateTimeOverlay,
  updateDateTimeOverlay,
  addImageOverlay,
  updateImageOverlay,
  removeImageOverlay,
}: OsdSettingsProps) {
  const { t } = useTranslation();

  // Datetime is a single switch per stream — the first overlay (if any) is the
  // canonical one the toggle drives. Toggling off disables it (config kept) so
  // re-enabling restores format/corner/font without re-entering them.
  const datetimeOverlay = datetimeOverlays[0] ?? null;
  const datetimeEnabled = datetimeOverlay?.enabled ?? false;

  if (loading) {
    return <OsdSettingsSkeleton />;
  }

  const streamTabs: { id: StreamName; label: string }[] = [
    { id: 'main', label: t('sys.media_settings.main_stream', 'Main Stream') },
    { id: 'sub', label: t('sys.media_settings.sub_stream', 'Sub Stream') },
    {
      id: 'third',
      label: t('sys.media_settings.third_stream', 'Third Stream'),
    },
  ];

  return (
    <ScrollArea type="auto" className="h-full">
      <SaveStatusIndicator status={saveStatus} />
      <div className="space-y-4">
        {/* Stream selector — drives the live Player (所见即所编辑) */}
        <section className="space-y-3">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-4 space-y-3">
              <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                <Video className="w-4 h-4" />
                {t('sys.media_settings.osd_stream_config', 'Stream')}
              </h3>

              <div className="flex gap-2">
                {streamTabs.map(tab => {
                  const isSelected = activeStream === tab.id;
                  return (
                    <button
                      key={tab.id}
                      type="button"
                      onClick={() => onStreamChange(tab.id)}
                      className={cn(
                        'flex-1 rounded-md border px-3 py-2 transition-colors text-center',
                        isSelected
                          ? 'border-primary bg-primary/10 text-primary'
                          : 'border-border hover:bg-muted/30 text-foreground'
                      )}
                    >
                      <div className="text-xs font-medium">{tab.label}</div>
                    </button>
                  );
                })}
              </div>

              {/* Drag hint — the interaction handles live on the video, not here */}
              <p className="text-xs text-muted-foreground leading-relaxed">
                {t(
                  'sys.media_settings.osd_sidebar_hint',
                  'Drag overlays directly on the live video. Double-click a text box to type. Drag the corner handle to resize.'
                )}
              </p>
            </CardContent>
          </Card>
        </section>

        {/* Text Overlays Section */}
        <section className="space-y-3">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-4 space-y-3">
              <div className="flex items-center justify-between">
                <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                  <TypeIcon className="w-4 h-4" />
                  {t('sys.media_settings.osd_text_overlay', 'Text Overlay')}
                </h3>
                <Button
                  variant="ghost"
                  size="sm"
                  onClick={addTextOverlay}
                  className="h-7 text-xs"
                >
                  <Plus className="w-3 h-3 mr-1" /> {t('sys.common.add', 'Add')}
                </Button>
              </div>
              {textOverlays.map(o => (
                <div
                  key={o.id}
                  className={cn(
                    'space-y-2 p-2 rounded-md border transition-all',
                    selectedId === o.id ? 'border-primary/40' : 'border-border',
                    !o.enabled && 'opacity-50'
                  )}
                  onClick={() => onSelect(o.id)}
                >
                  <FontSizeRow
                    value={o.font_size ?? 32}
                    onChange={v => updateTextOverlay(o.id, { font_size: v })}
                    onDone={() => onSelect(null)}
                  />
                  <div className="flex items-center gap-2">
                    <Input
                      value={o.text ?? ''}
                      onChange={e => updateTextOverlay(o.id, { text: e.target.value })}
                      onBlur={e => {
                        // 失焦（焦点移出当前 overlay 项）时取消选中，player 文字框→完成态
                        const next = e.relatedTarget as Node | null;
                        const item =                          e.currentTarget.parentElement?.parentElement;
                        if (!next || !item || !item.contains(next)) onSelect(null);
                      }}
                      className="h-7 text-xs flex-1"
                      placeholder={t(
                        'sys.media_settings.osd_text_placeholder',
                        'Text'
                      )}
                    />
                    <Button
                      aria-label={t(
                        'sys.common.toggle_visibility',
                        'Show or hide text overlay'
                      )}
                      variant="ghost"
                      size="sm"
                      onClick={() => updateTextOverlay(o.id, { enabled: !o.enabled })}
                      className="h-7 w-7 p-0"
                    >
                      {o.enabled ? (
                        <Eye className="w-3.5 h-3.5" />
                      ) : (
                        <EyeOff className="w-3.5 h-3.5 text-muted-foreground" />
                      )}
                    </Button>
                    <Button
                      aria-label={t('sys.common.delete', 'Delete text overlay')}
                      variant="ghost"
                      size="sm"
                      onClick={() => removeTextOverlay(o.id)}
                      className="h-7 w-7 p-0 text-destructive"
                    >
                      <Trash2 className="w-3.5 h-3.5" />
                    </Button>
                  </div>
                </div>
              ))}
              {textOverlays.length === 0 && (
                <div className="text-xs text-muted-foreground text-center py-2">
                  {t('sys.media_settings.osd_no_text', 'No text overlays')}
                </div>
              )}
            </CardContent>
          </Card>
        </section>

        {/* DateTime Overlay — single on/off switch. At most one datetime overlay
            per stream; the switch enable/disables it (config is preserved when
            toggled off so re-enabling restores format/corner/font). */}
        <section className="space-y-3">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-4 space-y-3">
              <div className="flex items-center justify-between">
                <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                  <ClockIcon className="w-4 h-4" />
                  {t(
                    'sys.media_settings.osd_datetime_overlay',
                    'DateTime Overlay'
                  )}
                </h3>
                <Switch
                  aria-label={t(
                    'sys.media_settings.osd_datetime_overlay',
                    'Enable date and time overlay'
                  )}
                  checked={datetimeEnabled}
                  onCheckedChange={checked => {
                    if (checked) {
                      if (datetimeOverlay) {
                        updateDateTimeOverlay(datetimeOverlay.id, {
                          enabled: true,
                        });
                      } else {
                        addDateTimeOverlay();
                      }
                    } else if (datetimeOverlay) {
                      updateDateTimeOverlay(datetimeOverlay.id, {
                        enabled: false,
                      });
                    }
                  }}
                />
              </div>
              {datetimeEnabled && datetimeOverlay && (
                <div className="space-y-2 p-2 rounded-md bg-muted/50">
                  <FontSizeRow
                    value={datetimeOverlay.font_size ?? 28}
                    onChange={v => updateDateTimeOverlay(datetimeOverlay.id, {
                        font_size: v,
                      })}
                    onDone={() => onSelect(null)}
                  />
                  <div>
                    <Label className="text-[10px] text-muted-foreground">
                      {t('sys.media_settings.osd_corner_align', 'Corner')}
                    </Label>
                    <div className="grid grid-cols-2 gap-1 mt-1">
                      {CORNERS.map(c => {
                        const active =                          (datetimeOverlay.h_align ?? 0) === c.hAlign
                          && (datetimeOverlay.v_align ?? 0) === c.vAlign;
                        return (
                          <Button
                            key={c.key}
                            type="button"
                            variant={active ? 'default' : 'outline'}
                            size="sm"
                            className="h-6 text-[10px]"
                            onClick={e => {
                              e.stopPropagation();
                              updateDateTimeOverlay(datetimeOverlay.id, {
                                x: c.x,
                                y: c.y,
                                h_align: c.hAlign,
                                v_align: c.vAlign,
                              });
                            }}
                          >
                            {t(
                              `sys.media_settings.osd_corner_${c.key}`,
                              c.key.toUpperCase()
                            )}
                          </Button>
                        );
                      })}
                    </div>
                  </div>
                </div>
              )}
            </CardContent>
          </Card>
        </section>

        {/* Image Overlays Section */}
        <section className="space-y-3">
          <Card className="shadow-sm bg-background">
            <CardContent className="p-4 space-y-3">
              <div className="flex items-center justify-between">
                <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                  <ImageIcon className="w-4 h-4" />
                  {t('sys.media_settings.osd_image_overlay', 'Image Overlay')}
                </h3>
                <div className="flex items-center gap-2">
                  <span className="text-[10px] text-muted-foreground tabular-nums">
                    {imageOverlays.length}/{MAX_IMAGE_OVERLAYS}
                  </span>
                  <Button
                    variant="ghost"
                    size="sm"
                    onClick={addImageOverlay}
                    disabled={imageOverlays.length >= MAX_IMAGE_OVERLAYS}
                    className="h-7 text-xs"
                  >
                    <Plus className="w-3 h-3 mr-1" /> {t('sys.common.add', 'Add')}
                  </Button>
                </div>
              </div>
              {imageOverlays.length >= MAX_IMAGE_OVERLAYS && (
                <p className="text-xs text-muted-foreground">
                  {t(
                    'sys.media_settings.osd_image_max_count',
                    'Up to {{max}} images',
                    { max: MAX_IMAGE_OVERLAYS }
                  )}
                </p>
              )}
              {imageOverlays.map(o => (
                <div
                  key={o.id}
                  className={cn(
                    'space-y-2 p-2 rounded-md transition-all',
                    selectedId === o.id ? 'ring-1 ring-primary/40' : '',
                    !o.enabled && 'opacity-50'
                  )}
                  onClick={() => onSelect(o.id)}
                >
                  <div className="flex items-center gap-2">
                    <Button
                      variant="outline"
                      size="sm"
                      className="h-7 w-0 min-w-0 flex-1 overflow-hidden text-xs gap-1.5 justify-start"
                      title={
                        o.image_path
                          ? `${o.image_path.split('/').pop()}${
                              o.image_size
                                ? ` - ${formatImageFileSize(o.image_size)}`
                                : ''
                            }`
                          : undefined
                      }
                      onClick={() => {
                        const input = document.getElementById(
                          `osd-img-${o.id}`
                        ) as HTMLInputElement | null;
                        input?.click();
                      }}
                    >
                      <Upload className="w-3 h-3" />
                      <span className="block min-w-0 flex-1 truncate text-left">
                        {o.image_path
                          ? `${o.image_path.split('/').pop()}${
                              o.image_size
                                ? ` - ${formatImageFileSize(o.image_size)}`
                                : ''
                            }`
                          : t(
                              'sys.media_settings.osd_upload_image',
                              'Upload Image'
                            )}
                      </span>
                    </Button>
                    <input
                      id={`osd-img-${o.id}`}
                      type="file"
                      accept=".png,.bmp"
                      className="hidden"
                      onChange={async e => {
                        const file = e.target.files?.[0];
                        if (!file) return;
                        if (file.size > MAX_IMAGE_SIZE_BYTES) {
                          toast.error(
                            t(
                              'sys.media_settings.osd_image_too_large',
                              'Image must be {{max}} MB or smaller',
                              { max: MAX_IMAGE_SIZE_BYTES / (1024 * 1024) }
                            )
                          );
                          e.target.value = '';
                          return;
                        }
                        try {
                          const result = await uploadOsdImage(file);
                          updateImageOverlay(o.id, {
                            image_path: result.path,
                            image_size: result.size,
                            enabled: true,
                          });
                        } catch {
                          toast.error(
                            t(
                              'sys.media_settings.osd_upload_failed',
                              'Upload failed'
                            )
                          );
                        }
                        e.target.value = '';
                      }}
                    />
                    <Button
                      aria-label={t(
                        'sys.common.toggle_visibility',
                        'Show or hide image overlay'
                      )}
                      variant="ghost"
                      size="sm"
                      onClick={() => updateImageOverlay(o.id, { enabled: !o.enabled })}
                      className="h-7 w-7 p-0"
                    >
                      {o.enabled ? (
                        <Eye className="w-3.5 h-3.5" />
                      ) : (
                        <EyeOff className="w-3.5 h-3.5 text-muted-foreground" />
                      )}
                    </Button>
                    <Button
                      aria-label={t(
                        'sys.common.delete',
                        'Delete image overlay'
                      )}
                      variant="ghost"
                      size="sm"
                      onClick={() => removeImageOverlay(o.id)}
                      className="h-7 w-7 p-0 text-destructive"
                    >
                      <Trash2 className="w-3.5 h-3.5" />
                    </Button>
                  </div>
                </div>
              ))}
              {imageOverlays.length === 0 && (
                <div className="text-xs text-muted-foreground text-center py-2">
                  {t('sys.media_settings.osd_no_image', 'No image overlays')}
                </div>
              )}
            </CardContent>
          </Card>
        </section>
      </div>
    </ScrollArea>
  );
}
