import { useTranslation } from 'react-i18next';
import { Shield, Plus, Trash2, Sparkles } from 'lucide-react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Slider } from '@/components/ui/slider';
import { ScrollArea } from '@/components/ui/scroll-area';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Card, CardContent } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import type { PrivacyMaskController } from '../hooks/usePrivacyMaskConfig';
import { PrivacyMaskSkeleton } from './OverlaySkeletons';

// The two DPM render modes only: pixelization (wire "mosaic") and color (wire "overlay").
const DPM_MODES = ['mosaic', 'overlay'] as const;
// DPM targets grouped by the coarse-vs-fine mutual-exclusion pairs. Each pair
// renders as a single-choice segmented control (pick one granularity or none),
// so the exclusivity is visible instead of a silent checkbox replacement.
const DPM_GROUPS: { key: string; labels: string[] }[] = [
  { key: 'people', labels: ['person', 'face'] },
  { key: 'vehicles', labels: ['vehicle', 'license_plate'] },
];

type Props = PrivacyMaskController;

export default function PrivacyMaskSettings(props: Props) {
  const { t } = useTranslation();
  const {
    config,
    loading,
    drawingRegion,
    pendingPoints,
    activeRegion,
    setActiveRegion,
    handleToggleEnabled,
    handleBlurRadiusChange,
    handleColorChange,
    handleDeleteRegion,
    handleToggleRegion,
    handleDpmToggle,
    handleDpmModeChange,
    handleDpmLabelToggle,
    handleDpmColorChange,
    startDrawing,
  } = props;

  if (loading) {
    return <PrivacyMaskSkeleton />;
  }

  if (!config) return null;

  const colorHex = `#${((config.color >> 16) & 0xff).toString(16).padStart(2, '0')}${((config.color >> 8) & 0xff).toString(16).padStart(2, '0')}${(config.color & 0xff).toString(16).padStart(2, '0')}`;

  const dpmColorHex = `#${((config.dpm_color >> 16) & 0xff).toString(16).padStart(2, '0')}${((config.dpm_color >> 8) & 0xff).toString(16).padStart(2, '0')}${(config.dpm_color & 0xff).toString(16).padStart(2, '0')}`;

  // Stale configs may carry dpm_mode="blur" (removed mode) — clamp to a valid
  // mode for display; the device self-heals the moment the user picks a mode.
  const activeDpmMode = (DPM_MODES as readonly string[]).includes(
    config.dpm_mode || ''
  )
    ? config.dpm_mode!
    : 'mosaic';

  // Parse the CSV wire format once for the segmented-control selection state.
  const selectedDpmLabels = (config.dpm_labels || '')
    .split(',')
    .map(s => s.trim())
    .filter(Boolean);

  return (
    <ScrollArea type="auto" className="h-full">
      <div className="space-y-4">
        {/* 大标题：隐私遮挡（主开关 + 遮挡样式 + 区域遮挡 子项）。
            DPM（AI 自动遮挡）独立成卡，不受主开关影响。 */}
        {/* <SaveStatusIndicator status={saveStatus} /> */}
        <Card className="shadow-sm bg-background">
          <CardContent className="p-4 space-y-3">
            <div className="flex items-center justify-between gap-3">
              <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                <Shield className="w-4 h-4" />
                {t('sys.media_settings.privacy_mask')}
              </h3>
              <Switch
                aria-label={t(
                  'sys.media_settings.privacy_mask',
                  'Enable privacy mask'
                )}
                checked={config.enabled}
                onCheckedChange={handleToggleEnabled}
              />
            </div>
            <p className="text-xs text-muted-foreground">
              {config.enabled
                ? t(
                    'sys.media_settings.privacy_mask_master_hint_on',
                    'Static mask enabled. Draw polygon regions below to cover fixed areas of the frame.'
                  )
                : t(
                    'sys.media_settings.privacy_mask_master_hint_off',
                    'Cover fixed areas of the frame with a static polygon mask.'
                  )}
            </p>

            {/* 主开关开启 → 遮挡样式 + 区域遮挡 两个子项 */}
            {config.enabled && (
              <>
                {/* 子项：遮挡样式 */}
                <div className="space-y-3 border-t border-border pt-3">
                  <h4 className="text-sm font-normal text-muted-foreground flex items-center gap-1.5">
                    {t('sys.media_settings.privacy_mask_style')}
                  </h4>

                  <div className="space-y-3">
                    <div className="space-y-1.5">
                      <Label className="text-xs">
                        {t('sys.media_settings.privacy_mask_blur')}
                      </Label>
                      <div className="flex items-center space-x-3">
                        <Slider
                          aria-label={t(
                            'sys.media_settings.privacy_mask_blur',
                            'Blur strength'
                          )}
                          value={[config.blur_radius]}
                          onValueChange={handleBlurRadiusChange}
                          min={0}
                          max={64}
                          step={1}
                          className="flex-1"
                        />
                      </div>
                    </div>

                    {config.blur_radius === 0 && (
                      <div className="flex items-center justify-between">
                        <Label className="text-xs">
                          {t('sys.media_settings.privacy_mask_color')}
                        </Label>
                        <div className="flex items-center gap-2">
                          <span className="text-xs text-muted-foreground font-mono">
                            {colorHex.toUpperCase()}
                          </span>
                          <input
                            aria-label={t(
                              'sys.media_settings.privacy_mask_color',
                              'Mask color'
                            )}
                            type="color"
                            value={colorHex}
                            onChange={handleColorChange}
                            className="h-6 w-6 rounded border border-border bg-transparent cursor-pointer p-0"
                          />
                        </div>
                      </div>
                    )}
                  </div>
                </div>

                {/* 子项：区域遮挡（原独立卡片，归入隐私遮挡大标题下） */}
                <div className="space-y-3 border-t border-border pt-3">
                  <div className="flex items-center justify-between gap-3">
                    <h4 className="text-sm font-normal text-muted-foreground flex items-center gap-1.5">
                      {t('sys.media_settings.privacy_mask_regions')}
                    </h4>
                    <Button
                      variant="outline"
                      size="sm"
                      className="h-7 text-xs"
                      onClick={startDrawing}
                      disabled={drawingRegion}
                    >
                      <Plus className="w-3 h-3 mr-1" />
                      {t('sys.media_settings.privacy_mask_add_region')}
                    </Button>
                  </div>

                  <p className="text-xs text-muted-foreground">
                    {drawingRegion
                      ? pendingPoints.length < 3
                        ? t(
                            'sys.media_settings.privacy_mask_draw_hint_progress',
                            'Click to place vertices (need 3 min). Double-click to close the polygon.'
                          )
                        : t(
                            'sys.media_settings.privacy_mask_draw_hint_close',
                            'Double-click to close the polygon.'
                          )
                      : t(
                          'sys.media_settings.privacy_mask_draw_hint_idle',
                          'Click "Add Region" then click on the live video to place vertices. Drag a vertex to fine-tune.'
                        )}
                  </p>

                  <div className="space-y-2">
                    {config.regions.length === 0 && !drawingRegion && (
                      <p className="text-xs text-muted-foreground text-center py-2">
                        {t(
                          'sys.media_settings.privacy_mask_no_regions',
                          'No mask regions yet'
                        )}
                      </p>
                    )}
                    {config.regions.map(region => (
                      <div
                        key={region.id}
                        className={`flex items-center justify-between gap-2 p-2 rounded-md border transition-colors cursor-pointer ${
                          activeRegion === region.id
                            ? 'border-primary/40'
                            : 'border-border'
                        }`}
                        onClick={() => setActiveRegion(region.id)}
                        role="button"
                        tabIndex={0}
                        onKeyDown={event => {
                          if (event.key === 'Enter' || event.key === ' ') {
                            event.preventDefault();
                            setActiveRegion(region.id);
                          }
                        }}
                      >
                        <div className="flex items-center gap-2 min-w-0">
                          {/* <GripVertical className="h-3 w-3 text-muted-foreground shrink-0" /> */}
                          <Switch
                            aria-label={`${region.name} ${t('sys.common.enabled', 'Enabled')}`}
                            checked={region.enabled}
                            onCheckedChange={v => handleToggleRegion(region.id, v)}
                          />
                          <span className="text-sm truncate">
                            {region.name}
                          </span>
                          <span className="text-[10px] text-muted-foreground shrink-0">
                            ({region.points_x.length} pts)
                          </span>
                        </div>
                        <Button
                          aria-label={t(
                            'sys.common.delete',
                            'Delete mask region'
                          )}
                          variant="ghost"
                          size="icon"
                          className="h-6 w-6 shrink-0"
                          onClick={e => {
                            e.stopPropagation();
                            handleDeleteRegion(region.id);
                          }}
                        >
                          <Trash2 className="h-3 w-3 text-destructive" />
                        </Button>
                      </div>
                    ))}
                  </div>
                </div>
              </>
            )}
          </CardContent>
        </Card>

        {/* Dynamic Privacy Mask (AI auto-mask). Independent of the static
            polygon mask — reachable on its own so users can enable AI masking
            without first drawing a static region. */}
        <Card className="shadow-sm bg-background">
          <CardContent className="p-4 space-y-3">
            <div className="flex items-center justify-between gap-3">
              <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                <Sparkles className="w-4 h-4" />
                {t('sys.media_settings.privacy_mask_dpm')}
              </h3>
              <Switch
                aria-label={t(
                  'sys.media_settings.privacy_mask_dpm',
                  'Enable dynamic privacy mask'
                )}
                checked={config.dpm_enabled}
                onCheckedChange={handleDpmToggle}
              />
            </div>
            <p className="text-xs text-muted-foreground">
              {config.dpm_enabled
                ? t(
                    'sys.media_settings.privacy_mask_dpm_hint_on',
                    'AI detects and masks selected targets in real time. Independent of the static polygon mask.'
                  )
                : t(
                    'sys.media_settings.privacy_mask_dpm_hint_off',
                    'Enable AI auto-masking for detected targets. Independent of the static polygon mask.'
                  )}
            </p>

            {config.dpm_enabled && (
              <div className="space-y-3">
                <div className="space-y-2">
                  <Label className="text-xs">
                    {t('sys.media_settings.privacy_mask_dpm_labels')}
                  </Label>
                  <p className="text-xs text-muted-foreground">
                    {t('sys.media_settings.privacy_mask_dpm_labels_hint')}
                  </p>
                  <div className="space-y-3 pt-1">
                    {DPM_GROUPS.map(group => (
                      <div key={group.key} className="space-y-1.5">
                        <div className="text-[11px] text-muted-foreground">
                          {t(
                            `sys.media_settings.privacy_mask_dpm_group_${group.key}`
                          )}
                        </div>
                        <div className="grid grid-cols-2 rounded-md border border-border overflow-hidden">
                          {group.labels.map((labelKey, idx) => {
                            const selected =                              selectedDpmLabels.includes(labelKey);
                            return (
                              <button
                                key={labelKey}
                                type="button"
                                role="radio"
                                aria-checked={selected}
                                aria-label={t(
                                  `sys.media_settings.privacy_mask_dpm_label_${labelKey}`
                                )}
                                onClick={() => handleDpmLabelToggle(labelKey)}
                                className={`flex items-center justify-center gap-1.5 px-2 py-2 text-xs transition-colors ${
                                  idx > 0 ? 'border-l border-border' : ''
                                } ${
                                  selected
                                    ? 'bg-primary/10 text-primary'
                                    : 'hover:bg-accent text-foreground'
                                }`}
                              >
                                <span
                                  className={`h-2 w-2 rounded-full transition-colors ${
                                    selected
                                      ? 'bg-primary'
                                      : 'bg-muted-foreground/40'
                                  }`}
                                />
                                {t(
                                  `sys.media_settings.privacy_mask_dpm_label_${labelKey}`
                                )}
                              </button>
                            );
                          })}
                        </div>
                      </div>
                    ))}
                  </div>
                </div>

                <div className="space-y-1.5">
                  <Label className="text-xs">
                    {t('sys.media_settings.privacy_mask_dpm_mode')}
                  </Label>
                  <Select
                    value={activeDpmMode}
                    onValueChange={handleDpmModeChange}
                  >
                    <SelectTrigger className="w-full">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      {DPM_MODES.map(mode => (
                        <SelectItem key={mode} value={mode}>
                          {t(`sys.media_settings.privacy_mask_dpm_${mode}`)}
                        </SelectItem>
                      ))}
                    </SelectContent>
                  </Select>
                </div>

                {activeDpmMode === 'overlay' && (
                  <div className="flex items-center justify-between">
                    <Label className="text-xs">
                      {t('sys.media_settings.privacy_mask_dpm_color')}
                    </Label>
                    <div className="flex items-center gap-2">
                      <span className="text-xs text-muted-foreground font-mono">
                        {dpmColorHex.toUpperCase()}
                      </span>
                      <input
                        aria-label={t(
                          'sys.media_settings.privacy_mask_dpm_color',
                          'Dynamic mask color'
                        )}
                        type="color"
                        value={dpmColorHex}
                        onChange={handleDpmColorChange}
                        className="h-6 w-6 rounded border border-border bg-transparent cursor-pointer p-0"
                      />
                    </div>
                  </div>
                )}
              </div>
            )}
          </CardContent>
        </Card>
      </div>
    </ScrollArea>
  );
}
