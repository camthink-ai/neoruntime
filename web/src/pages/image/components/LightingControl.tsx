import { useState, useCallback, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Lightbulb, Minus, Plus } from 'lucide-react';
import { Card, CardContent } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Slider } from '@/components/ui/slider';
import { Switch } from '@/components/ui/switch';
import { cn } from '@/lib/utils';
import { LightingControlSkeleton } from './DeviceControlSkeletons';
import {
  useDeviceStatus,
  useSetLight,
  useSetIrLed,
} from '@/hooks/useDeviceControl';

const clamp = (v: number, min: number, max: number) => Math.min(max, Math.max(min, v));
const DEFAULT_LEVEL = 50;

interface IrBrightnessSliderProps {
  level: number;
  disabled: boolean;
  onLevelChange: (level: number) => void;
  onLevelCommit: (level: number) => void;
  onStep: (direction: -1 | 1) => void;
}

function IrBrightnessSlider({
  level,
  disabled,
  onLevelChange,
  onLevelCommit,
  onStep,
}: IrBrightnessSliderProps) {
  return (
    <div className={cn('flex items-center gap-2', disabled && 'opacity-50')}>
      <Button
        type="button"
        variant="outline"
        className="h-10 w-10 shrink-0 rounded-xl"
        disabled={disabled || level <= 0}
        onClick={() => onStep(-1)}
      >
        <Minus className="h-4 w-4" />
      </Button>
      <Slider
        value={[level]}
        min={0}
        max={100}
        step={1}
        disabled={disabled}
        onValueChange={values => {
          const v =            Array.isArray(values) && Number.isFinite(values[0])
              ? Math.round(values[0])
              : level;
          onLevelChange(clamp(v, 0, 100));
        }}
        onValueCommit={values => {
          const raw =            Array.isArray(values) && Number.isFinite(values[0])
              ? Math.round(values[0])
              : level;
          const normalized = clamp(raw, 0, 100);
          onLevelChange(normalized);
          onLevelCommit(normalized);
        }}
      />
      <Button
        type="button"
        variant="outline"
        className="h-10 w-10 shrink-0 rounded-xl"
        disabled={disabled || level >= 100}
        onClick={() => onStep(1)}
      >
        <Plus className="h-4 w-4" />
      </Button>
    </div>
  );
}

export default function LightingControl() {
  const { t } = useTranslation();
  const { data: status, isLoading } = useDeviceStatus();
  const setLight = useSetLight();
  const setIrLed = useSetIrLed();

  const [nearLevel, setNearLevel] = useState(
    status?.white_light_level ?? DEFAULT_LEVEL
  );
  const [farLevel, setFarLevel] = useState(DEFAULT_LEVEL);

  useEffect(() => {
    if (status?.white_light_level != null && status.white_light_level > 0) {
      setNearLevel(status.white_light_level);
    }
  }, [status?.white_light_level]);

  useEffect(() => {
    if (status?.ir_led_level != null) {
      setFarLevel(status.ir_led_level > 0 ? status.ir_led_level : 0);
    }
  }, [status?.ir_led_level]);

  const handleNearLevelCommit = useCallback(
    async (level: number) => {
      try {
        await setLight.mutateAsync(level);
      } catch {
        toast.error(
          t('sys.device.lighting.set_failed', 'Failed to update setting')
        );
      }
    },
    [setLight, t]
  );

  const handleFarLevelCommit = useCallback(
    async (level: number) => {
      try {
        await setIrLed.mutateAsync(level);
      } catch {
        toast.error(
          t('sys.device.lighting.set_failed', 'Failed to update setting')
        );
      }
    },
    [setIrLed, t]
  );

  const handleNearStep = async (direction: -1 | 1) => {
    const next = clamp(nearLevel + direction * 5, 0, 100);
    setNearLevel(next);
    await handleNearLevelCommit(next);
  };

  const handleFarStep = async (direction: -1 | 1) => {
    const next = clamp(farLevel + direction * 5, 0, 100);
    setFarLevel(next);
    await handleFarLevelCommit(next);
  };

  const handleNearIrToggle = async (on: boolean) => {
    const level = on
      ? clamp(nearLevel > 0 ? nearLevel : DEFAULT_LEVEL, 1, 100)
      : 0;
    if (on) {
      setNearLevel(level);
    }
    try {
      await setLight.mutateAsync(level);
    } catch {
      toast.error(
        t('sys.device.lighting.set_failed', 'Failed to update setting')
      );
    }
  };

  const handleFarIrToggle = async (on: boolean) => {
    const level = on
      ? clamp(farLevel > 0 ? farLevel : DEFAULT_LEVEL, 1, 100)
      : 0;
    if (on) {
      setFarLevel(level);
    } else {
      setFarLevel(0);
    }
    try {
      await setIrLed.mutateAsync(level);
    } catch {
      toast.error(
        t('sys.device.lighting.set_failed', 'Failed to update setting')
      );
    }
  };

  if (isLoading) {
    return <LightingControlSkeleton />;
  }

  const nearIrOn = (status?.white_light_level ?? 0) > 0;
  const farIrOn = (status?.ir_led_level ?? 0) > 0;

  return (
    <Card className="shadow-sm bg-background">
      <CardContent className="space-y-5 p-5">
        <h3 className="flex items-center gap-1.5 text-sm font-bold text-muted-foreground">
          <Lightbulb className="h-4 w-4" />
          {t('sys.device.lighting.title', 'IR Light Control')}
        </h3>

        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <span className="text-sm text-muted-foreground">
              {t('sys.device.lighting.ir_near', 'Near IR')}
            </span>
            <Switch
              checked={nearIrOn}
              onCheckedChange={handleNearIrToggle}
              disabled={setLight.isPending}
            />
          </div>
          <div className="space-y-2">
            <div className="flex items-center justify-between text-sm text-muted-foreground">
              <span>
                {t('sys.device.lighting.near_brightness', 'Near IR Brightness')}
              </span>
              <Badge
                variant="secondary"
                className="px-4 py-1 font-mono text-[10px]"
              >
                {nearLevel}%
              </Badge>
            </div>
            <IrBrightnessSlider
              level={nearLevel}
              disabled={!nearIrOn || setLight.isPending}
              onLevelChange={setNearLevel}
              onLevelCommit={handleNearLevelCommit}
              onStep={handleNearStep}
            />
          </div>
        </div>

        <div className="space-y-3">
          <div className="flex items-center justify-between">
            <span className="text-sm text-muted-foreground">
              {t('sys.device.lighting.ir_far', 'Far IR')}
            </span>
            <Switch
              checked={farIrOn}
              onCheckedChange={handleFarIrToggle}
              disabled={setIrLed.isPending}
            />
          </div>
          <div className="space-y-2">
            <div className="flex items-center justify-between text-sm text-muted-foreground">
              <span>
                {t('sys.device.lighting.far_brightness', 'Far IR Brightness')}
              </span>
              <Badge
                variant="secondary"
                className="px-4 py-1 font-mono text-[10px]"
              >
                {farLevel}%
              </Badge>
            </div>
            <IrBrightnessSlider
              level={farLevel}
              disabled={!farIrOn || setIrLed.isPending}
              onLevelChange={setFarLevel}
              onLevelCommit={handleFarLevelCommit}
              onStep={handleFarStep}
            />
          </div>
        </div>
      </CardContent>
    </Card>
  );
}
