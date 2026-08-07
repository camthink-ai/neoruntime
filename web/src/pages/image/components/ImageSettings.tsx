import { useState, useEffect, useCallback, useRef } from 'react';
import { useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Loader2, Sparkles } from 'lucide-react';
import { Label } from '@/components/ui/label';
import { Switch } from '@/components/ui/switch';
import { Slider } from '@/components/ui/slider';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Input } from '@/components/ui/input';
import { Card, CardContent } from '@/components/ui/card';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import { ImageIcon, RotateCw } from 'lucide-react';
import ImageSettingsSkeleton from './ImageSettingsSkeleton';
import {
  fetchISPConfig,
  updateISPConfig,
  fetchTransformConfig,
  updateTransformConfig,
  fetchProfiles,
  switchProfile,
  type ISPConfig,
  type TransformConfig,
} from '@/services/settings';

// Client-side allowlist mirroring camera-daemon's SwitchProfile guard. The
// device returns the full profile list (Daylight/HDR/detection variants…); we
// only surface the three AI ISP Basic profiles for UI switching.
const AI_ISP_PROFILE_RE = /^AI_ISP_Gen([123])_Basic$/;

const AI_ISP_GEN_LABEL_KEY: Record<string, string> = {
  '1': 'sys.media_settings.isp_profile_gen1',
  '2': 'sys.media_settings.isp_profile_gen2',
  '3': 'sys.media_settings.isp_profile_gen3',
};

const AWB_OPTIONS = [
  { value: '-1', labelKey: 'sys.media_settings.awb_auto' },
  { value: '0', labelKey: 'sys.media_settings.awb_a' },
  { value: '1', labelKey: 'sys.media_settings.awb_d50' },
  { value: '2', labelKey: 'sys.media_settings.awb_d65' },
  { value: '3', labelKey: 'sys.media_settings.awb_d75' },
  { value: '4', labelKey: 'sys.media_settings.awb_tl84' },
  { value: '5', labelKey: 'sys.media_settings.awb_f12' },
  { value: '6', labelKey: 'sys.media_settings.awb_cwf' },
] as const;

// Rate-limit profile switches: every switch restarts the GStreamer pipeline,
// so cap it to one per 5s and prompt the user if they try sooner.
const SWITCH_COOLDOWN_MS = 5000;
// Safety cap on how long the profile dropdown stays greyed after a switch, in
// case the player never reports wv_work(true) (not mounted, stream stalled…).
const PLAYER_RELOAD_SAFETY_MS = 15000;

export default function ImageSettings() {
  const { t } = useTranslation();
  const queryClient = useQueryClient();

  const [isp, setIsp] = useState<ISPConfig | null>(null);
  const [transform, setTransform] = useState<TransformConfig | null>(null);
  // True while a v1/media/transform request is in flight. The rotation/flip
  // dropdowns stay disabled until the call resolves so the user can't queue a
  // second change while the first is still being applied.
  const [transformSaving, setTransformSaving] = useState(false);
  const [loading, setLoading] = useState(true);

  // AI ISP profile state. `profiles` is the device list filtered down to the
  // Gen{1,2,3}_Basic allowlist; `currentProfile` is the device's active
  // profile (may be a non-AI-ISP profile, in which case the Select shows a
  // placeholder). `switching` disables the control during a pipeline restart.
  const [aiProfiles, setAiProfiles] = useState<string[]>([]);
  const [currentProfile, setCurrentProfile] = useState<string>('');
  const [switching, setSwitching] = useState(false);
  const [pendingProfile, setPendingProfile] = useState<string | null>(null);
  // True while the player reloads after a profile switch (pipeline restart →
  // reconnect → first frame). The profile dropdown stays greyed in this window.
  const [playerReloading, setPlayerReloading] = useState(false);

  // Timestamp of the last committed switch; switch attempts within
  // SWITCH_COOLDOWN_MS are rejected with a prompt.
  const lastSwitchAtRef = useRef(0);
  const playerReloadTimeoutRef = useRef<ReturnType<typeof setTimeout> | null>(
    null
  );

  const isInSwitchCooldown = () => Date.now() - lastSwitchAtRef.current < SWITCH_COOLDOWN_MS;

  // Remembers the last AI ISP Gen profile the user had active so toggling
  // AI ISP off→on restores it instead of always jumping to the default. Set
  // whenever the device lands on an AI ISP profile (load + switch success).
  const lastAiGenRef = useRef<string>('');
  const aiEnabled = AI_ISP_PROFILE_RE.test(currentProfile);
  useEffect(() => {
    if (aiEnabled) {
      lastAiGenRef.current = currentProfile;
    }
  }, [aiEnabled, currentProfile]);

  const loadData = useCallback(async () => {
    try {
      const [ispConfig, transformConfig, profileResp] = await Promise.all([
        fetchISPConfig(),
        fetchTransformConfig(),
        fetchProfiles(),
      ]);
      setIsp(ispConfig);
      setTransform(transformConfig);
      setCurrentProfile(profileResp.current_profile);
      setAiProfiles(
        profileResp.profiles.filter(p => AI_ISP_PROFILE_RE.test(p))
      );
    } catch {
      // handled by request interceptor
    } finally {
      setLoading(false);
    }
  }, [t]);

  useEffect(() => {
    loadData();
  }, [loadData]);

  // Clear the player-reload grey-out as soon as the new stream produces its
  // first frame (wv_work detail === true). The safety timeout set in
  // handleSwitchProfile is the fallback if this event never arrives.
  useEffect(() => {
    const onWvWork = (e: Event) => {
      if ((e as CustomEvent<boolean>).detail !== true) return;
      setPlayerReloading(false);
      if (playerReloadTimeoutRef.current) {
        clearTimeout(playerReloadTimeoutRef.current);
        playerReloadTimeoutRef.current = null;
      }
    };
    window.addEventListener('wv_work', onWvWork);
    return () => window.removeEventListener('wv_work', onWvWork);
  }, []);

  useEffect(
    () => () => {
      if (playerReloadTimeoutRef.current) {
        clearTimeout(playerReloadTimeoutRef.current);
        playerReloadTimeoutRef.current = null;
      }
    },
    []
  );

  // Switching restarts the GStreamer pipeline for ~interrupt_ms; the player
  // listens for the dispatched event and reconnects after that window.
  const handleSwitchProfile = useCallback(
    async (profileName: string) => {
      setSwitching(true);
      // Start the 5s cooldown from the moment a switch is committed (confirm).
      lastSwitchAtRef.current = Date.now();
      try {
        const result = await switchProfile(profileName);
        if (result.success) {
          setCurrentProfile(profileName);
          toast.success(t('sys.media_settings.isp_profile_switched'));
          // HAL reset the ISP gate on profile switch — reload ISP sliders so
          // the UI reflects the new profile's defaults instead of stale vals.
          try {
            const ispConfig = await fetchISPConfig();
            setIsp(ispConfig);
          } catch {
            // non-fatal: sliders just stay stale until next manual load
          }
          window.dispatchEvent(
            new CustomEvent('aipc:media-profile-changed', {
              detail: { interrupt_ms: result.interrupt_ms },
            })
          );
          // Grey the dropdown while the player reloads. Cleared by wv_work(true)
          // on first frame, or by the safety timeout below as a fallback.
          setPlayerReloading(true);
          if (playerReloadTimeoutRef.current) {
            clearTimeout(playerReloadTimeoutRef.current);
          }
          playerReloadTimeoutRef.current = setTimeout(() => {
            setPlayerReloading(false);
            playerReloadTimeoutRef.current = null;
          }, Math.max(result.interrupt_ms || 0, 0) + PLAYER_RELOAD_SAFETY_MS);
        } else {
          const restricted = /thermal|restrict/i.test(result.message);
          toast.error(
            restricted
              ? t('sys.media_settings.isp_profile_restricted')
              : t('sys.media_settings.isp_profile_switch_failed')
          );
        }
      } catch {
        toast.error(t('sys.media_settings.isp_profile_switch_failed'));
      } finally {
        setSwitching(false);
        setPendingProfile(null);
      }
    },
    [t]
  );

  const ispDebounceRef = useRef<
    Record<string, { value: number; timer: ReturnType<typeof setTimeout> }>
  >({});

  const handleISPChange = async (field: string, value: number | boolean) => {
    if (!isp) return;

    const prevValue = isp[field as keyof ISPConfig];
    setIsp(prev => (prev ? { ...prev, [field]: value } : prev));

    const payload: Record<string, number | boolean> = { [field]: value };

    if (typeof value === 'number') {
      const existing = ispDebounceRef.current[field];
      if (existing) {
        clearTimeout(existing.timer);
      }
      ispDebounceRef.current[field] = {
        value,
        timer: setTimeout(async () => {
          try {
            await updateISPConfig(payload);
          } catch {
            toast.error(t('sys.media_settings.save_failed'));
            setIsp(prev => (prev ? { ...prev, [field]: prevValue } : prev));
          }
        }, 300),
      };
      return;
    }

    try {
      await updateISPConfig(payload);
    } catch {
      toast.error(t('sys.media_settings.save_failed'));
      setIsp(prev => (prev ? { ...prev, [field]: prevValue } : prev));
    }
  };

  const transformDebounceRef = useRef<
    Record<string, { value: number; timer: ReturnType<typeof setTimeout> }>
  >({});

  const refreshMediaStatus = useCallback(() => {
    queryClient.invalidateQueries({ queryKey: ['mediaStatus'] });
    [1000, 3000].forEach(delay => {
      window.setTimeout(() => {
        queryClient.invalidateQueries({ queryKey: ['mediaStatus'] });
      }, delay);
    });
  }, [queryClient]);

  const handleTransformChange = async (
    field: string,
    value: number | boolean
  ) => {
    if (!transform) return;

    const prevValue = transform[field as keyof TransformConfig];
    setTransform(prev => (prev ? { ...prev, [field]: value } : prev));

    if (typeof value === 'number') {
      const existing = transformDebounceRef.current[field];
      if (existing) {
        clearTimeout(existing.timer);
      }
      transformDebounceRef.current[field] = {
        value,
        timer: setTimeout(async () => {
          setTransformSaving(true);
          try {
            await updateTransformConfig({ [field]: value });
            const actual = await fetchTransformConfig();
            setTransform(actual);
            refreshMediaStatus();
          } catch {
            toast.error(t('sys.media_settings.save_failed'));
            setTransform(prev => (prev ? { ...prev, [field]: prevValue } : prev));
          } finally {
            setTransformSaving(false);
          }
        }, 300),
      };
      return;
    }

    setTransformSaving(true);
    try {
      await updateTransformConfig({ [field]: value });
      const actual = await fetchTransformConfig();
      setTransform(actual);
      refreshMediaStatus();
    } catch {
      toast.error(t('sys.media_settings.save_failed'));
      setTransform(prev => (prev ? { ...prev, [field]: prevValue } : prev));
    } finally {
      setTransformSaving(false);
    }
  };

  if (loading) {
    return <ImageSettingsSkeleton />;
  }

  const manualMode = isp?.manual_mode ?? false;
  const autoExposure = isp?.auto_exposure ?? true;

  return (
    <div className="flex flex-col gap-4">
      {/* AI ISP Profile Card */}
        <Card className="shadow-sm bg-background">
          <CardContent className="p-5 space-y-4">
            <div className="flex items-center justify-between gap-3">
              <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
                <Sparkles className="w-4 h-4" />
                {t('sys.media_settings.isp_profile_label')}
              </h3>
              <div className="flex items-center gap-2">
                <Label
                  htmlFor="ai-isp-enable"
                  className="text-xs text-muted-foreground"
                >
                  {t('sys.media_settings.isp_profile_enable')}
                </Label>
                <Switch
                  id="ai-isp-enable"
                  checked={aiEnabled}
                  disabled={switching || aiProfiles.length === 0}
                  onCheckedChange={checked => {
                    if (isInSwitchCooldown()) {
                      toast.warning(
                        t('sys.media_settings.isp_profile_switch_cooldown')
                      );
                      return;
                    }
                    if (checked) {
                      // Restore the last Gen profile the user had, else default
                      // to Gen3 (highest quality) when available, else the first
                      // AI ISP profile the device exposes.
                      const target =                        lastAiGenRef.current
                        || aiProfiles.find(p => p.includes('Gen3'))
                        || aiProfiles[0];
                      if (target) setPendingProfile(target);
                    } else {
                      setPendingProfile('Daylight_Basic');
                    }
                  }}
                />
              </div>
            </div>
            {aiEnabled && (
              <>
                <p className="text-xs text-muted-foreground">
                  {t('sys.media_settings.isp_profile_hint')}
                </p>
                <div className="space-y-1.5">
                  <Label className="text-xs">
                    {t('sys.media_settings.isp_profile_label')}
                  </Label>
                  {aiProfiles.length === 0 ? (
                    <p className="text-xs text-muted-foreground">
                      {t('sys.media_settings.isp_profile_unavailable')}
                    </p>
                  ) : (
                    <Select
                      value={currentProfile}
                      onValueChange={v => {
                        if (isInSwitchCooldown()) {
                          toast.warning(
                            t('sys.media_settings.isp_profile_switch_cooldown')
                          );
                          return;
                        }
                        setPendingProfile(v);
                      }}
                      disabled={switching || playerReloading}
                    >
                      <SelectTrigger className="w-full">
                        <SelectValue />
                      </SelectTrigger>
                      <SelectContent>
                        {aiProfiles.map(p => {
                          const gen = p.match(AI_ISP_PROFILE_RE)?.[1] ?? '';
                          return (
                            <SelectItem key={p} value={p}>
                              {t(AI_ISP_GEN_LABEL_KEY[gen] ?? '')}
                            </SelectItem>
                          );
                        })}
                      </SelectContent>
                    </Select>
                  )}
                </div>
              </>
            )}
          </CardContent>
        </Card>

        {/* ISP Settings Card */}
        <Card className="shadow-sm bg-background">
          <CardContent className="p-5 space-y-4">
            <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
              <ImageIcon className="w-4 h-4" />
              {t('sys.media_settings.isp_settings')}
            </h3>

            <div className="space-y-3">
              {/* Manual Mode */}
              <div className="flex items-center justify-between">
                <div className="space-y-0.5">
                  <Label className="text-xs">
                    {t('sys.media_settings.manual_mode')}
                  </Label>
                  <p className="text-xs text-muted-foreground">
                    {t('sys.media_settings.manual_mode_hint')}
                  </p>
                </div>
                <Switch
                  checked={manualMode}
                  onCheckedChange={v => handleISPChange('manual_mode', v)}
                />
              </div>

              {/* Manual Mode Dependent Fields */}
              {manualMode && (
                <>
                  <div className="pt-2 border-t">
                    <p className="text-xs font-medium text-muted-foreground">
                      {t('sys.media_settings.manual_mode')}
                    </p>
                  </div>

                  {/* Brightness */}
                  <div className="space-y-1.5">
                    <Label className="text-xs">
                      {t('sys.media_settings.brightness')}
                    </Label>
                    <div className="flex items-center space-x-3">
                      <Slider
                        value={[isp?.brightness ?? 50]}
                        onValueChange={v => handleISPChange('brightness', v[0])}
                        min={0}
                        max={100}
                        step={1}
                        className="flex-1"
                      />
                      <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                        {isp?.brightness ?? 50}
                      </span>
                    </div>
                  </div>

                  {/* Contrast */}
                  <div className="space-y-1.5">
                    <Label className="text-xs">
                      {t('sys.media_settings.contrast')}
                    </Label>
                    <div className="flex items-center space-x-3">
                      <Slider
                        value={[isp?.contrast ?? 50]}
                        onValueChange={v => handleISPChange('contrast', v[0])}
                        min={0}
                        max={100}
                        step={1}
                        className="flex-1"
                      />
                      <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                        {isp?.contrast ?? 50}
                      </span>
                    </div>
                  </div>

                  {/* Saturation */}
                  <div className="space-y-1.5">
                    <Label className="text-xs">
                      {t('sys.media_settings.saturation')}
                    </Label>
                    <div className="flex items-center space-x-3">
                      <Slider
                        value={[isp?.saturation ?? 50]}
                        onValueChange={v => handleISPChange('saturation', v[0])}
                        min={0}
                        max={100}
                        step={1}
                        className="flex-1"
                      />
                      <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                        {isp?.saturation ?? 50}
                      </span>
                    </div>
                  </div>

                  {/* Sharpness */}
                  <div className="space-y-1.5">
                    <Label className="text-xs">
                      {t('sys.media_settings.sharpness')}
                    </Label>
                    <div className="flex items-center space-x-3">
                      <Slider
                        value={[isp?.sharpness ?? 50]}
                        onValueChange={v => handleISPChange('sharpness', v[0])}
                        min={0}
                        max={100}
                        step={1}
                        className="flex-1"
                      />
                      <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                        {isp?.sharpness ?? 50}
                      </span>
                    </div>
                  </div>

                  {/* Auto Exposure */}
                  <div className="flex items-center justify-between">
                    <Label className="text-xs">
                      {t('sys.media_settings.auto_exposure')}
                    </Label>
                    <Switch
                      checked={autoExposure}
                      onCheckedChange={v => handleISPChange('auto_exposure', v)}
                    />
                  </div>

                  {/* Exposure Time — only when AE off */}
                  {!autoExposure && (
                    <div className="space-y-1.5">
                      <Label className="text-xs">
                        {t('sys.media_settings.exposure_time_us')}
                      </Label>
                      <div className="flex items-center space-x-3">
                        <Slider
                          value={[isp?.exposure_time_us ?? 8000]}
                          onValueChange={v => handleISPChange('exposure_time_us', v[0])}
                          min={2000}
                          max={33000}
                          step={100}
                          className="flex-1"
                        />
                        <Input
                          type="number"
                          value={isp?.exposure_time_us ?? 8000}
                          onChange={e => handleISPChange(
                              'exposure_time_us',
                              Number(e.target.value)
                            )}
                          min={2000}
                          max={33000}
                          className="w-20 h-7 text-xs"
                        />
                      </div>
                    </div>
                  )}

                  {/* Gain — only when AE off */}
                  {!autoExposure && (
                    <div className="space-y-1.5">
                      <Label className="text-xs">
                        {t('sys.media_settings.gain')}
                      </Label>
                      <div className="flex items-center space-x-3">
                        <Slider
                          value={[isp?.gain ?? 1]}
                          onValueChange={v => handleISPChange('gain', v[0])}
                          min={1}
                          max={3800}
                          step={1}
                          className="flex-1"
                        />
                        <Input
                          type="number"
                          value={isp?.gain ?? 1}
                          onChange={e => handleISPChange('gain', Number(e.target.value))}
                          min={1}
                          max={3800}
                          className="w-20 h-7 text-xs"
                        />
                      </div>
                    </div>
                  )}

                  {/* Backlight Compensation — only when AE on */}
                  {autoExposure && (
                    <div className="space-y-1.5">
                      <Label className="text-xs">
                        {t('sys.media_settings.backlight')}
                      </Label>
                      <div className="flex items-center space-x-3">
                        <Slider
                          value={[isp?.backlight ?? 0]}
                          onValueChange={v => handleISPChange('backlight', v[0])}
                          min={0}
                          max={100}
                          step={1}
                          className="flex-1"
                        />
                        <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                          {isp?.backlight ?? 0}
                        </span>
                      </div>
                    </div>
                  )}

                  {/* WDR Value — manual mode only */}
                  <div className="space-y-1.5">
                    <Label className="text-xs">
                      {t('sys.media_settings.wdr_mode')}
                    </Label>
                    <div className="flex items-center space-x-3">
                      <Slider
                        value={[isp?.wdr_value ?? 0]}
                        onValueChange={v => handleISPChange('wdr_value', v[0])}
                        min={0}
                        max={100}
                        step={1}
                        className="flex-1"
                      />
                      <span className="text-xs text-muted-foreground w-10 text-right tabular-nums">
                        {isp?.wdr_value ?? 0}
                      </span>
                    </div>
                  </div>
                </>
              )}

              {/* Powerline Frequency — always visible */}
              <div className="space-y-1.5">
                <Label className="text-xs">
                  {t('sys.media_settings.powerline_freq')}
                </Label>
                <Select
                  value={String(isp?.powerline_freq ?? 0)}
                  onValueChange={v => handleISPChange('powerline_freq', Number(v))}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="0">
                      {t('sys.media_settings.powerline_off')}
                    </SelectItem>
                    <SelectItem value="1">
                      {t('sys.media_settings.powerline_50hz')}
                    </SelectItem>
                    <SelectItem value="2">
                      {t('sys.media_settings.powerline_60hz')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {/* AWB Mode — always visible */}
              <div className="space-y-1.5">
                <Label className="text-xs">
                  {t('sys.media_settings.awb_index')}
                </Label>
                <Select
                  value={String(isp?.awb_index ?? -1)}
                  onValueChange={v => handleISPChange('awb_index', Number(v))}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    {AWB_OPTIONS.map(opt => (
                      <SelectItem key={opt.value} value={opt.value}>
                        {t(opt.labelKey)}
                      </SelectItem>
                    ))}
                  </SelectContent>
                </Select>
              </div>
            </div>
          </CardContent>
        </Card>

        {/* Transform Settings Card */}
        <Card className="shadow-sm bg-background">
          <CardContent className="p-5 space-y-4">
            <h3 className="text-sm font-bold text-muted-foreground flex items-center gap-1.5">
              <RotateCw className="w-4 h-4" />
              {t('sys.media_settings.transform')}
            </h3>

            <div className="space-y-3">
              {/* Rotation */}
              <div className="space-y-1.5">
                <Label className="text-xs">
                  {t('sys.media_settings.rotation')}
                </Label>
                <Select
                  value={String(transform?.rotation ?? 0)}
                  onValueChange={v => handleTransformChange('rotation', Number(v))}
                  disabled={transformSaving}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="0">
                      {t('sys.media_settings.rotation_0')}
                    </SelectItem>
                    <SelectItem value="1">
                      {t('sys.media_settings.rotation_90')}
                    </SelectItem>
                    <SelectItem value="2">
                      {t('sys.media_settings.rotation_180')}
                    </SelectItem>
                    <SelectItem value="3">
                      {t('sys.media_settings.rotation_270')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {/* Flip */}
              <div className="space-y-1.5">
                <Label className="text-xs">
                  {t('sys.media_settings.flip')}
                </Label>
                <Select
                  value={String(transform?.flip ?? 0)}
                  onValueChange={v => handleTransformChange('flip', Number(v))}
                  disabled={transformSaving}
                >
                  <SelectTrigger className="w-full">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="0">
                      {t('sys.media_settings.flip_none')}
                    </SelectItem>
                    <SelectItem value="1">
                      {t('sys.media_settings.flip_horizontal')}
                    </SelectItem>
                    <SelectItem value="2">
                      {t('sys.media_settings.flip_vertical')}
                    </SelectItem>
                    <SelectItem value="3">
                      {t('sys.media_settings.flip_both')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {/* Dewarp */}
              <div className="flex items-center justify-between">
                <Label className="text-xs">
                  {t('sys.media_settings.dewarp')}
                </Label>
                <Switch
                  checked={transform?.dewarp ?? false}
                  onCheckedChange={v => handleTransformChange('dewarp', v)}
                />
              </div>

              {/* Grayscale */}
              <div className="flex items-center justify-between">
                <Label className="text-xs">
                  {t('sys.media_settings.grayscale')}
                </Label>
                <Switch
                  checked={transform?.grayscale ?? false}
                  onCheckedChange={v => handleTransformChange('grayscale', v)}
                />
              </div>

              {/* DIS - Digital Image Stabilization */}
              <div className="flex items-center justify-between">
                <div className="space-y-0.5">
                  <Label className="text-xs">
                    {t('sys.media_settings.dis')}
                  </Label>
                  <p className="text-xs text-muted-foreground">
                    {t('sys.media_settings.dis_hint')}
                  </p>
                </div>
                <Switch
                  checked={transform?.dis ?? false}
                  onCheckedChange={v => handleTransformChange('dis', v)}
                />
              </div>

              {/* EIS - Electronic Image Stabilization */}
              <div className="flex items-center justify-between">
                <div className="space-y-0.5">
                  <Label className="text-xs">
                    {t('sys.media_settings.eis')}
                  </Label>
                  <p className="text-xs text-muted-foreground">
                    {t('sys.media_settings.eis_hint')}
                  </p>
                </div>
                <Switch
                  checked={transform?.eis ?? false}
                  onCheckedChange={v => handleTransformChange('eis', v)}
                />
              </div>
            </div>
          </CardContent>
        </Card>

        {/* AI ISP switch confirm dialog — pipeline restart interrupts video */}
        <AlertDialog
          open={pendingProfile !== null}
          onOpenChange={open => {
            if (!open && !switching) setPendingProfile(null);
          }}
        >
          <AlertDialogContent>
            <AlertDialogHeader>
              <AlertDialogTitle>
                {t('sys.media_settings.isp_profile_switch_confirm')}
              </AlertDialogTitle>
              <AlertDialogDescription>
                {t('sys.media_settings.isp_profile_switch_confirm_hint')}
              </AlertDialogDescription>
            </AlertDialogHeader>
            <AlertDialogFooter>
              <AlertDialogCancel disabled={switching}>
                {t('sys.common.cancel')}
              </AlertDialogCancel>
              <AlertDialogAction
                disabled={switching}
                onClick={e => {
                  e.preventDefault();
                  if (pendingProfile) handleSwitchProfile(pendingProfile);
                }}
              >
                {switching && (
                  <Loader2 className="w-3.5 h-3.5 mr-1.5 animate-spin" />
                )}
                {t('sys.common.confirm')}
              </AlertDialogAction>
            </AlertDialogFooter>
          </AlertDialogContent>
        </AlertDialog>
    </div>
  );
}
