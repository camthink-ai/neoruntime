import { useEffect, useMemo, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import type { TFunction } from 'i18next';
import { toast } from 'sonner';
import {
  AlertTriangle,
  Aperture,
  Crosshair,
  Info,
  Loader2,
  RotateCcw,
} from 'lucide-react';
import {
  useAutofocusStatus,
  useLensGotoZoomRatio,
  useLensStatus,
  useOneshotAutofocus,
  useSetFocusLevel,
  useStartZoomFollow,
} from '@/hooks/useDeviceControl';
import type { LensStatus } from '@/services/api/device';
import { MotorState } from '@/services/api/device';
import { Button } from '@/components/ui/button';
import { Card, CardContent } from '@/components/ui/card';
// import { Separator } from '@/components/ui/separator';
import { LensControlSkeleton } from './DeviceControlSkeletons';
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip';
import MotorAxisControl from './MotorAxisControl';

// ── Constants ─────────────────────────────────────────────────────────
// AF0832 table-backed optical range.

const ZOOM_DISPLAY_MIN = 1.0;
const ZOOM_DISPLAY_MAX = 2.88;

// Fine-grained +/- and slider steps (percent of range per click): zoom 0.5%
// ≈ 12 steps ≈ 0.006x; focus 1% (FG2009: 1% of the 600-step window ≈ 6 steps).
const ZOOM_STEP_PERCENT = 0.5;
const FOCUS_STEP_PERCENT = 1;

// FG2009 focus window: after each zoom move the daemon drives focus onto the
// INF tracking curve, and the slider narrows to +/-300 steps around that
// landing point for comfortable fine-tuning (full travel is 2453 steps).
// At the travel ends the window shifts inward so the span stays 600.
const FOCUS_WINDOW_STEPS = 300;
const FG2009_FOCUS_STEP_PERCENT = 1; // 1% of the 600-step window ≈ 6 steps

const clamp01 = (value: number) => Math.min(1, Math.max(0, value));

// ── Helpers ───────────────────────────────────────────────────────────

function zoomLevelFromStatus(s: LensStatus): number {
  const { zoom_pos, zoom_limit } = s;
  const range = zoom_limit.max_pos - zoom_limit.min_pos;
  if (!Number.isFinite(range) || range <= 0) return 0;
  return clamp01((zoom_pos - zoom_limit.min_pos) / range);
}

function focusLevelFromStatus(s: LensStatus): number {
  const { focus_pos, focus_limit } = s;
  const range = focus_limit.max_pos - focus_limit.min_pos;
  if (!Number.isFinite(range) || range <= 0) return 0;
  return clamp01((focus_pos - focus_limit.min_pos) / range);
}

// Backend autofocus status messages (camera-daemon autofocus_controller.cpp)
// that surface in toasts. Map each known message to a localized key so the
// raw English string is never shown verbatim; unknown messages pass through.
const AF_STATUS_MESSAGE_KEYS: Record<string, string> = {
  'focus acquired': 'sys.ptz.af_focus_acquired',
  'low confidence focus': 'sys.ptz.af_low_confidence',
  'coarse scan failed': 'sys.ptz.af_coarse_scan_failed',
};

function translateAfStatusMessage(message: string, t: TFunction): string {
  const key = AF_STATUS_MESSAGE_KEYS[message.trim()];
  return key ? t(key, message) : message;
}

// ── Main Component ────────────────────────────────────────────────────

export default function LensControl() {
  const { t } = useTranslation();
  const {
    data: lensStatus,
    isLoading: isDeviceLoading,
    refetch: refetchLensStatus,
  } = useLensStatus();

  // FG2009 = open-loop lens: pure manual zoom/focus, no autofocus stack.
  // An old backend without lens_model reports undefined → AF0832 behavior.
  const isFg2009 = lensStatus?.lens_model === 'fg2009';
  const zoomDisplayMax = lensStatus?.zoom_ratio_range?.max && lensStatus.zoom_ratio_range.max > 1
    ? lensStatus.zoom_ratio_range.max
    : ZOOM_DISPLAY_MAX;

  const { data: autofocusStatus } = useAutofocusStatus({ enabled: !isFg2009 });

  const oneshotAutofocus = useOneshotAutofocus();
  const startZoomFollow = useStartZoomFollow();
  const lensGotoZoomRatio = useLensGotoZoomRatio();
  const setFocusLevel = useSetFocusLevel();

  const [zoomPercent, setZoomPercent] = useState(0);
  const [focusPercent, setFocusPercent] = useState(0);
  const previousAfBusy = useRef(false);

  // ── Derived state ─────────────────────────────────────────────────

  const zLevel = useMemo(() => {
    if (isFg2009) {
      // Open-loop lens: the reported optical ratio comes straight from the
      // position model — there is no autofocus anchor to consult.
      const ratio = lensStatus?.zoom_ratio;
      if (ratio != null && ratio >= ZOOM_DISPLAY_MIN && ratio <= zoomDisplayMax) {
        return clamp01((ratio - ZOOM_DISPLAY_MIN) / (zoomDisplayMax - ZOOM_DISPLAY_MIN));
      }
      return lensStatus ? zoomLevelFromStatus(lensStatus) : 0;
    }
    if (
      autofocusStatus?.anchor_valid
      && autofocusStatus.effective_ratio >= ZOOM_DISPLAY_MIN
      && autofocusStatus.effective_ratio <= ZOOM_DISPLAY_MAX
    ) {
      return clamp01(
        (autofocusStatus.effective_ratio - ZOOM_DISPLAY_MIN)
          / (ZOOM_DISPLAY_MAX - ZOOM_DISPLAY_MIN)
      );
    }
    return lensStatus ? zoomLevelFromStatus(lensStatus) : 0;
  }, [autofocusStatus?.anchor_valid, autofocusStatus?.effective_ratio, isFg2009, lensStatus, zoomDisplayMax]);
  const fLevel = useMemo(
    () => (lensStatus ? focusLevelFromStatus(lensStatus) : 0),
    [lensStatus]
  );

  // FG2009 focus window: the zoom follow (daemon-side, INF tracking curve)
  // lands focus on a known point; the slider narrows to +/-300 steps around
  // it. Re-anchored when zoom_ratio changes (new follow landing), on mount,
  // or when focus ends up outside the window (e.g. MCU reinit park).
  const focusPos = lensStatus?.focus_pos;
  const focusTravel = lensStatus
    ? lensStatus.focus_limit.max_pos - lensStatus.focus_limit.min_pos
    : 0;
  const [focusCenter, setFocusCenter] = useState<number | null>(null);
  const prevZoomRatio = useRef<number | null>(null);
  useEffect(() => {
    if (!isFg2009 || focusPos == null) return;
    const zr = lensStatus?.zoom_ratio ?? null;
    const zoomMoved = prevZoomRatio.current != null && zr != null
      && zr !== prevZoomRatio.current;
    prevZoomRatio.current = zr;
    setFocusCenter(prev => {
      if (prev == null || zoomMoved) return focusPos;
      return Math.abs(focusPos - prev) > FOCUS_WINDOW_STEPS ? focusPos : prev;
    });
  }, [isFg2009, focusPos, lensStatus?.zoom_ratio]);

  const focusWindow = useMemo(() => {
    if (!isFg2009 || !lensStatus || focusTravel <= 0 || focusCenter == null) {
      return null;
    }
    const { min_pos, max_pos } = lensStatus.focus_limit;
    let lo = focusCenter - FOCUS_WINDOW_STEPS;
    let hi = focusCenter + FOCUS_WINDOW_STEPS;
    // Travel-end bounce: keep the full 600-step span inside [min, max].
    if (lo < min_pos) {
      lo = min_pos;
      hi = Math.min(max_pos, min_pos + 2 * FOCUS_WINDOW_STEPS);
    }
    if (hi > max_pos) {
      hi = max_pos;
      lo = Math.max(min_pos, max_pos - 2 * FOCUS_WINDOW_STEPS);
    }
    return { lo, hi };
  }, [isFg2009, lensStatus, focusTravel, focusCenter]);

  // Window mode maps the slider onto [lo, hi]; otherwise the full travel.
  const focusSliderLevel = useMemo(() => {
    if (!lensStatus) return fLevel;
    if (focusWindow && focusPos != null) {
      const span = focusWindow.hi - focusWindow.lo;
      if (span > 0) return clamp01((focusPos - focusWindow.lo) / span);
    }
    return fLevel;
  }, [lensStatus, fLevel, focusWindow, focusPos]);

  // Sync local percent from server when it changes
  const prevZ = useMemo(() => zLevel, [zLevel]);
  useEffect(() => {
    setZoomPercent(prevZ * 100);
  }, [prevZ]);

  const prevF = useMemo(() => focusSliderLevel, [focusSliderLevel]);
  useEffect(() => {
    setFocusPercent(prevF * 100);
  }, [prevF]);

  const zoomRatioDisplay = useMemo(() => {
    const level = clamp01(zoomPercent / 100);
    return ZOOM_DISPLAY_MIN + (zoomDisplayMax - ZOOM_DISPLAY_MIN) * level;
  }, [zoomPercent, zoomDisplayMax]);

  const focusDisplay = useMemo(() => {
    // FG2009 window mode: window-relative percent — 0% at the slider's left
    // end, 100% at the right; the +/-300 geometry stays under the hood.
    // AF0832: percent over the full travel. Both are plain percents.
    const level = isFg2009 ? focusSliderLevel : fLevel;
    return `${(level * 100).toFixed(1)}%`;
  }, [fLevel, focusSliderLevel, isFg2009]);

  const afBusy = autofocusStatus?.busy ?? false;
  const isOneshotAF = afBusy
    && (autofocusStatus?.operation === 'oneshot' || autofocusStatus?.operation === 'startup');

  // Motor states
  const zoomState: MotorState = lensStatus?.zoom_state ?? MotorState.NoCfg;
  const focusState: MotorState = lensStatus?.focus_state ?? MotorState.NoCfg;

  const hasMotorError =    zoomState === MotorState.Error || focusState === MotorState.Error;

  // AF0832 reports Running/ResetZero while it homes at boot, which deserves
  // the full "initializing" card below. The FG2009 is open-loop: every
  // ordinary zoom/focus move reports Running too, so there the same state
  // only disables the sliders until the motors stop — no card takeover.
  const motorsRunning =    zoomState === MotorState.Running
    || focusState === MotorState.Running;
  const isMotorInitializing =    !isFg2009
    && !isDeviceLoading
    && !afBusy
    && !hasMotorError
    && (motorsRunning
      || zoomState === MotorState.ResetZero
      || focusState === MotorState.ResetZero);
  const isMotorMoving = isFg2009 && !hasMotorError && motorsRunning;

  const isZoomBusy =    (isFg2009 ? lensGotoZoomRatio.isPending : startZoomFollow.isPending)
    || afBusy || isMotorInitializing || isMotorMoving || hasMotorError;
  const isFocusBusy =    setFocusLevel.isPending || afBusy || isMotorInitializing || isMotorMoving || hasMotorError;

  const canZoomIn =    lensStatus != null && lensStatus.zoom_pos < lensStatus.zoom_limit.max_pos;
  const canZoomOut =    lensStatus != null && lensStatus.zoom_pos > lensStatus.zoom_limit.min_pos;
  const canFocusNear =    lensStatus != null && lensStatus.focus_pos > lensStatus.focus_limit.min_pos;
  const canFocusFar =    lensStatus != null && lensStatus.focus_pos < lensStatus.focus_limit.max_pos;

  // ── Poll while motors are moving ─────────────────────────────────

  useEffect(() => {
    if (!isMotorInitializing && !isMotorMoving) return;
    const timer = setInterval(() => {
      refetchLensStatus();
    }, 1000);
    return () => clearInterval(timer);
  }, [isMotorInitializing, isMotorMoving, refetchLensStatus]);

  useEffect(() => {
    if (previousAfBusy.current && !afBusy && autofocusStatus?.job_id) {
      if (autofocusStatus.state === 'completed') {
        toast.success(t('sys.ptz.af_done', 'Focus completed'));
      } else if (autofocusStatus.state === 'failed') {
        toast.error(translateAfStatusMessage(autofocusStatus.message, t) || t('sys.ptz.oneshot_af_failed', 'Auto focus failed'));
      }
      refetchLensStatus();
    }
    previousAfBusy.current = afBusy;
  }, [afBusy, autofocusStatus?.job_id, autofocusStatus?.message, autofocusStatus?.state, refetchLensStatus, t]);

  // ── Handlers ──────────────────────────────────────────────────────

  const handleOneshotAutofocus = async () => {
    try {
      await oneshotAutofocus.mutateAsync();
    } catch (error: unknown) {
      const msg =        error instanceof Error
          ? error.message
          : (error as any)?.response?.data?.message
            || t('sys.ptz.oneshot_af_failed', 'Auto focus failed');
      toast.error(msg);
    }
  };

  const handleResetZoom = async () => {
    setZoomPercent(0);
    if (isFg2009) {
      await lensGotoZoomRatio.mutateAsync(ZOOM_DISPLAY_MIN);
      return;
    }
    await startZoomFollow.mutateAsync(ZOOM_DISPLAY_MIN);
  };

  // ── Render ────────────────────────────────────────────────────────

  if (isDeviceLoading) {
    return <LensControlSkeleton />;
  }

  if (isMotorInitializing) {
    return (
      <div className="space-y-6">
        <div className="flex min-h-[300px] flex-col items-center justify-center gap-3 p-6">
          <Loader2 className="w-8 h-8 animate-spin text-primary" />
          <div className="text-sm text-muted-foreground">
            {t('sys.ptz.lens_initializing', 'Lens initializing...')}
          </div>
          <div className="text-xs text-muted-foreground/60">
            {zoomState !== MotorState.Stopped
            && focusState !== MotorState.Stopped
              ? t('sys.ptz.motors_moving', 'Zoom & focus motors moving')
              : zoomState !== MotorState.Stopped
                ? t('sys.ptz.zoom_moving', 'Zoom motor moving')
                : t('sys.ptz.focus_moving', 'Focus motor moving')}
          </div>
        </div>
      </div>
    );
  }

  if (hasMotorError) {
    return (
      <div className="space-y-6">
        <div className="flex min-h-[300px] flex-col items-center justify-center gap-3 p-6">
          <AlertTriangle className="w-8 h-8 text-destructive" />
          <div className="text-sm font-medium text-destructive">
            {t('sys.ptz.motor_error', 'Motor error detected')}
          </div>
          <div className="text-xs text-muted-foreground text-center max-w-[280px]">
            {zoomState === MotorState.Error
              && focusState === MotorState.Error
              && t(
                'sys.ptz.motor_error_hint_both',
                'Zoom and focus motors reported an error. Try restarting device-control service.'
              )}
            {zoomState === MotorState.Error
              && focusState !== MotorState.Error
              && t(
                'sys.ptz.motor_error_hint_zoom',
                'Zoom motor reported an error. Try restarting device-control service.'
              )}
            {zoomState !== MotorState.Error
              && focusState === MotorState.Error
              && t(
                'sys.ptz.motor_error_hint_focus',
                'Focus motor reported an error. Try restarting device-control service.'
              )}
          </div>
        </div>
      </div>
    );
  }

  return (
    <Card className="shadow-sm bg-background">
      <CardContent className="space-y-5 p-4">
        <h3 className="flex items-center gap-1.5 text-sm font-bold text-muted-foreground">
          <Aperture className="h-4 w-4" />
          {t('sys.device.lens.title', 'Lens Control')}
          {isFg2009 && (
            <span
              className="rounded bg-muted px-1.5 py-0.5 text-[10px] font-semibold tracking-wide text-muted-foreground"
              title={t('sys.device.lens.model_badge', 'Factory-fitted lens model')}
            >
              FG2009
            </span>
          )}
        </h3>

        <div className="flex items-center justify-between">
          {isFg2009 ? (
            <span className="text-sm text-muted-foreground">
              {t('sys.device.lens.manual_only', 'Manual zoom & focus')}
            </span>
          ) : (
            <div className="flex items-center gap-1.5 text-sm text-muted-foreground">
              <span>{t('sys.ptz.oneshot_af', 'One-shot AF')}</span>
              <TooltipProvider delayDuration={200}>
                <Tooltip>
                  <TooltipTrigger asChild>
                    <Button
                      type="button"
                      variant="ghost"
                      size="icon"
                      className="h-7 w-7 text-muted-foreground hover:text-foreground"
                      aria-label={t(
                        'sys.ptz.oneshot_af_hint',
                        'Tap to trigger auto focus at current zoom position'
                      )}
                    >
                      <Info className="h-4 w-4" />
                    </Button>
                  </TooltipTrigger>
                  <TooltipContent side="top" className="text-xs max-w-[280px]">
                    {t(
                      'sys.ptz.oneshot_af_hint',
                      'Tap to trigger auto focus at current zoom position'
                    )}
                  </TooltipContent>
                </Tooltip>
              </TooltipProvider>
            </div>
          )}
          <div className="flex items-center gap-1">
            <TooltipProvider delayDuration={200}>
              <Tooltip>
                <TooltipTrigger asChild>
                  <Button
                    type="button"
                    variant="ghost"
                    size="icon"
                    className="h-8 w-8 text-muted-foreground hover:text-foreground"
                    disabled={isZoomBusy}
                    onClick={handleResetZoom}
                    aria-label={t('sys.ptz.reset_zoom', 'Reset to 1.0x')}
                  >
                    <RotateCcw className="h-3.5 w-3.5" />
                  </Button>
                </TooltipTrigger>
                <TooltipContent side="top" className="text-xs">
                  {t('sys.ptz.reset_zoom', 'Reset to 1.0x')}
                </TooltipContent>
              </Tooltip>
            </TooltipProvider>
            {!isFg2009 && (
              <Button
                type="button"
                variant="outline"
                size="icon"
                className="h-9 w-9 bg-[#f24a001a] border-transparent hover:bg-[#f24a001a]"
                disabled={isOneshotAF || isZoomBusy || isFocusBusy}
                onClick={handleOneshotAutofocus}
                aria-label={t('sys.ptz.oneshot_af', 'One-shot AF')}
              >
                {isOneshotAF ? (
                  <Loader2 className="w-4 h-4 animate-spin text-primary" />
                ) : (
                  <Crosshair className="w-4 h-4 text-primary" />
                )}
              </Button>
            )}
          </div>
        </div>

        {/* <Separator /> */}

        <MotorAxisControl
          label={t('sys.ptz.zoom', 'Zoom')}
          displayValue={`${zoomRatioDisplay.toFixed(2)}x`}
          level={zoomPercent}
          stepPercent={ZOOM_STEP_PERCENT}
          onLevelChange={setZoomPercent}
          onCommit={async level => {
            const ratio = ZOOM_DISPLAY_MIN
              + (zoomDisplayMax - ZOOM_DISPLAY_MIN) * clamp01(level);
            if (isFg2009) {
              await lensGotoZoomRatio.mutateAsync(ratio);
              return;
            }
            await startZoomFollow.mutateAsync(ratio);
          }}
          busy={isZoomBusy}
          canDecrement={canZoomOut}
          canIncrement={canZoomIn}
        />

        <MotorAxisControl
          label={t('sys.ptz.focus', 'Focus')}
          displayValue={focusDisplay}
          level={focusPercent}
          stepPercent={isFg2009 ? FG2009_FOCUS_STEP_PERCENT : FOCUS_STEP_PERCENT}
          onLevelChange={setFocusPercent}
          onCommit={async level => {
            if (focusWindow && focusTravel > 0) {
              // Window mode: map [0,1] back onto the window, then express
              // the target as an absolute level over the full travel.
              const steps = focusWindow.lo + level * (focusWindow.hi - focusWindow.lo);
              await setFocusLevel.mutateAsync(clamp01(steps / focusTravel));
              return;
            }
            await setFocusLevel.mutateAsync(level);
          }}
          busy={isFocusBusy}
          canDecrement={canFocusNear}
          canIncrement={canFocusFar}
        />
      </CardContent>
    </Card>
  );
}
