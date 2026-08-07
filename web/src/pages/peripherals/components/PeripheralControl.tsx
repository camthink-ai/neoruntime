import { useState, useEffect, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { Bell, Info } from 'lucide-react';
import { Card, CardContent } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Switch } from '@/components/ui/switch';
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip'
import { PeripheralControlSkeleton } from '@/pages/image/components/DeviceControlSkeletons'
import { cn } from '@/lib/utils'
import {
  useSetAlarmOut,
  useSetWiegand,
  useAlarmOutputs,
} from '@/hooks/useDeviceControl';
import {
  fetchAudioStatus,
  startAudioCapture,
  stopAudioCapture,
  setAudioConfig,
  type AudioStatus,
} from '@/services/settings';

const WIEGAND_CHANNELS = [0, 1] as const;
const ALARM_OUT_CHANNEL = 0;

type WiegandChannel = (typeof WIEGAND_CHANNELS)[number];

interface LevelToggleProps {
  high: boolean;
  disabled?: boolean;
  highLabel: string;
  lowLabel: string;
  onSelect: (high: boolean) => void;
}

function LevelToggle({
  high,
  disabled,
  highLabel,
  lowLabel,
  onSelect,
}: LevelToggleProps) {
  return (
    <div className="flex rounded-md border border-input p-0.5">
      <Button
        type="button"
        variant="ghost"
        size="sm"
        className={cn(
          'h-7 px-3 text-xs',
          high && 'bg-accent text-accent-foreground'
        )}
        disabled={disabled}
        onClick={() => {
          if (!high) onSelect(true);
        }}
      >
        {highLabel}
      </Button>
      <Button
        type="button"
        variant="ghost"
        size="sm"
        className={cn(
          'h-7 px-3 text-xs',
          !high && 'bg-accent text-accent-foreground'
        )}
        disabled={disabled}
        onClick={() => {
          if (high) onSelect(false);
        }}
      >
        {lowLabel}
      </Button>
    </div>
  );
}

export default function PeripheralControl() {
  const { t } = useTranslation();
  const setAlarmOut = useSetAlarmOut();
  const setWiegand = useSetWiegand();
  const { data: outputs, isLoading: outputsLoading } = useAlarmOutputs();

  const [audioStatus, setAudioStatus] = useState<AudioStatus | null>(null);
  const [audioLoading, setAudioLoading] = useState(true);
  const [audioToggling, setAudioToggling] = useState(false);
  const [playbackToggling, setPlaybackToggling] = useState(false);
  const [alarmInEnabled, setAlarmInEnabled] = useState(false);
  const [alarmInHigh, setAlarmInHigh] = useState(true);

  const channelLabel = useCallback(
    (name: string, channel: WiegandChannel) => `${name} ${t('sys.device.peripheral.channel', 'CH')}${channel}`,
    [t]
  );

  const notifyFailure = useCallback(() => {
    toast.error(
      t('sys.device.peripheral.set_failed', 'Failed to update setting')
    );
  }, [t]);

  useEffect(() => {
    if (!outputs) return;
    setAlarmInEnabled(outputs.alarm_out0 ?? false);
  }, [outputs]);

  const loadAudioStatus = useCallback(async () => {
    try {
      const status = await fetchAudioStatus();
      setAudioStatus(status);
    } catch {
      // handled by request interceptor
    } finally {
      setAudioLoading(false);
    }
  }, []);

  useEffect(() => {
    loadAudioStatus();
  }, [loadAudioStatus]);

  // Refresh on window focus so cross-page (Media) state stays fresh when the
  // user returns to the Peripheral tab.
  useEffect(() => {
    const onFocus = () => {
      loadAudioStatus();
    };
    window.addEventListener('focus', onFocus);
    return () => window.removeEventListener('focus', onFocus);
  }, [loadAudioStatus]);

  const handleAudioToggle = async (enabled: boolean) => {
    setAudioToggling(true);
    try {
      if (enabled) {
        await startAudioCapture({ volume: 1.0, mute: false });
      } else {
        await stopAudioCapture();
      }
      const status = await fetchAudioStatus();
      setAudioStatus(status);
      window.dispatchEvent(
        new CustomEvent('audio-capture-change', {
          detail: !!status.capturing,
        })
      );
    } catch {
      // handled by interceptor
    } finally {
      setAudioToggling(false);
    }
  };

  // Speaker-output (playback) gate switch. Persisted server-side as the
  // `playback_enabled` audio config flag; enforced by audio_talk.go so a
  // disabled speaker rejects push-to-talk regardless of the UI.
  const handlePlaybackToggle = async (enabled: boolean) => {
    setPlaybackToggling(true);
    try {
      await setAudioConfig({ playback_enabled: enabled });
      const status = await fetchAudioStatus();
      setAudioStatus(status);
      window.dispatchEvent(
        new CustomEvent('audio-playback-change', {
          detail: !!status.playback_enabled,
        })
      );
    } catch {
      // handled by interceptor
    } finally {
      setPlaybackToggling(false);
    }
  };

  const handleAlarmInSwitch = async (enabled: boolean) => {
    const prev = alarmInEnabled;
    setAlarmInEnabled(enabled);
    try {
      await setAlarmOut.mutateAsync({
        channel: ALARM_OUT_CHANNEL,
        enable: enabled,
      });
    } catch {
      setAlarmInEnabled(prev);
      notifyFailure();
    }
  };

  const handleWiegand = async (channel: WiegandChannel, enable: boolean) => {
    try {
      await setWiegand.mutateAsync({ channel, enable });
    } catch {
      notifyFailure();
    }
  };

  const alarmInHighLabel = t('sys.device.peripheral.alarm_in_high', 'High');
  const alarmInLowLabel = t('sys.device.peripheral.alarm_in_low', 'Low');
  const wiegandName = t('sys.device.peripheral.wiegand', 'Wiegand');

  if (outputsLoading || audioLoading) {
    return <PeripheralControlSkeleton />;
  }

  return (
    <Card className="shadow-sm bg-background">
      <CardContent className="p-6 md:p-8 space-y-6">
        <h3 className="flex items-center gap-2 text-sm font-semibold text-muted-foreground">
          <Bell className="h-4 w-4 text-primary" />
          {t('sys.device.peripheral.io_title', 'I/O Control')}
        </h3>

        <div className="flex items-center justify-between">
          <span className="flex items-center gap-2 text-sm text-muted-foreground">
            {t('sys.device.peripheral.mic_input', 'Mic Input')}
          </span>
          <Switch
            checked={!!audioStatus?.capturing}
            onCheckedChange={handleAudioToggle}
            disabled={audioToggling}
          />
        </div>

        <div className="flex items-center justify-between">
          <span className="flex items-center gap-2 text-sm text-muted-foreground">
            {t('sys.device.peripheral.speaker_output', 'Speaker Output')}
          </span>
          <Switch
            checked={!!audioStatus?.playback_enabled}
            onCheckedChange={handlePlaybackToggle}
            disabled={playbackToggling}
          />
        </div>

        <div className="flex items-center justify-between gap-3">
          <span className="text-sm text-muted-foreground">
            {t('sys.device.peripheral.alarm_in', 'Alarm Input')}
          </span>
          <Switch
            checked={alarmInEnabled}
            onCheckedChange={handleAlarmInSwitch}
            disabled={setAlarmOut.isPending}
          />
        </div>

        <div className="flex items-center justify-between gap-3">
          <div className="flex items-center gap-1.5">
            <span className="text-sm text-muted-foreground">
              {t('sys.device.peripheral.alarm_in_level', 'Alarm Input Level')}
            </span>
            <TooltipProvider delayDuration={200}>
              <Tooltip>
                <TooltipTrigger asChild>
                  <button
                    type="button"
                    className="inline-flex text-muted-foreground hover:text-foreground"
                    aria-label={t(
                      'sys.device.peripheral.alarm_in_hint',
                      'Alarm input level hint'
                    )}
                  >
                    <Info className="h-4 w-4" />
                  </button>
                </TooltipTrigger>
                <TooltipContent side="top" className="max-w-xs text-xs">
                  {t(
                    'sys.device.peripheral.alarm_in_hint',
                    'Configure the active trigger level for alarm input'
                  )}
                </TooltipContent>
              </Tooltip>
            </TooltipProvider>
          </div>
          <LevelToggle
            high={alarmInHigh}
            highLabel={alarmInHighLabel}
            lowLabel={alarmInLowLabel}
            onSelect={setAlarmInHigh}
          />
        </div>

        {WIEGAND_CHANNELS.map(ch => (
          <div key={`wout-${ch}`} className="flex items-center justify-between">
            <span className="text-sm text-muted-foreground">
              {channelLabel(wiegandName, ch)}
            </span>
            <Switch
              checked={
                ch === 0
                  ? (outputs?.wiegand0 ?? false)
                  : (outputs?.wiegand1 ?? false)
              }
              onCheckedChange={v => handleWiegand(ch, v)}
              disabled={setWiegand.isPending}
            />
          </div>
        ))}
      </CardContent>
    </Card>
  );
}
