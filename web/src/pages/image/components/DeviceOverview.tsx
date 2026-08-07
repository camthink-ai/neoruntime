import { useTranslation } from 'react-i18next';
import { Thermometer, Cpu, SunMoon } from 'lucide-react';
import { DeviceOverviewSkeleton } from './DeviceControlSkeletons';
import { useDeviceStatus } from '@/hooks/useDeviceControl';

const TEMP_WARN_SOC = 80;

interface DeviceOverviewProps {
  variant?: 'default' | 'compact';
}

export default function DeviceOverview({
  variant = 'default',
}: DeviceOverviewProps) {
  const { t } = useTranslation();
  const { data: status, isLoading } = useDeviceStatus();

  const isCompact = variant === 'compact';

  if (isLoading) {
    return <DeviceOverviewSkeleton compact={isCompact} />;
  }

  const socTemp = status?.soc_temp_c;
  const mcuVersion = status?.mcu_version;
  const ircutMode = status?.ircut_mode;
  const isNight = ircutMode === 'IRCUT_NIGHT' || ircutMode === 2;

  const tempColor =    socTemp == null
      ? 'text-muted-foreground'
      : socTemp >= TEMP_WARN_SOC
        ? 'text-red-500'
        : 'text-emerald-500';

  const metrics = [
    {
      label: t('sys.device.overview.soc_temp', 'SoC Temp'),
      value: socTemp != null ? `${socTemp.toFixed(1)}°C` : '--',
      icon: <Thermometer className="w-3.5 h-3.5" />,
      warn: socTemp != null && socTemp >= TEMP_WARN_SOC,
      color: tempColor,
    },
    {
      label: t('sys.media_settings.ir_cut', 'Day/Night Mode'),
      value: isNight
        ? t('sys.media_settings.ir_cut_night', 'Night')
        : t('sys.media_settings.ir_cut_day', 'Day'),
      icon: <SunMoon className="w-3.5 h-3.5" />,
      warn: false,
      color: 'text-foreground',
    },
    {
      label: t('sys.device.overview.mcu_version', 'MCU Ver'),
      value: mcuVersion || '--',
      icon: <Cpu className="w-3.5 h-3.5" />,
      warn: false,
      color: 'text-foreground',
    },
  ];

  return (
    <div className={`grid grid-cols-3 gap-3 ${isCompact ? '' : 'sm:gap-4'}`}>
      {metrics.map(m => (
        <div
          key={m.label}
          className="flex items-center gap-2.5 rounded-lg border px-3 py-2.5"
        >
          <span className={m.color}>{m.icon}</span>
          <div className="min-w-0">
            <div className="truncate text-xs text-muted-foreground">
              {m.label}
            </div>
            <div
              className={`text-sm font-semibold tabular-nums ${m.color} ${m.warn ? 'animate-pulse' : ''}`}
            >
              {m.value}
            </div>
          </div>
        </div>
      ))}
    </div>
  );
}
