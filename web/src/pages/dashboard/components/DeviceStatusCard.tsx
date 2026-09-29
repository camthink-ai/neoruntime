import { useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { Activity, Timer, Thermometer } from 'lucide-react';
import { useMonitorSnapshot } from '@/services/dashboard';
import { useDeviceClock } from '@/hooks/useDeviceClock';

const TEMP_WARN_SOC = 80;
const TEMP_WARN_BOARD = 80;

export default function DeviceStatusCard() {
  const { t } = useTranslation();
  const { systemTime, formattedClock } = useDeviceClock();

  // Temperature snapshot (cpu/board) — 由 useMonitorSnapshot 统一提供
  const { data: snapshot } = useMonitorSnapshot();

  const temps = snapshot?.temperatures;
  const socTemp = temps?.cpu;

  const hasWarning =    (temps?.board ?? 0) >= TEMP_WARN_BOARD || (socTemp ?? 0) >= TEMP_WARN_SOC;

  const uptimeDisplay = useMemo(() => {
    if (!systemTime?.uptime) return '-';
    const totalSeconds = systemTime.uptime;
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    if (days > 0) return `${days}${t('sys.dashboard.uptime_day')} ${hours}${t('sys.dashboard.uptime_hour')} ${minutes}${t('sys.dashboard.uptime_minute')}`;
    if (hours > 0) return `${hours}${t('sys.dashboard.uptime_hour')} ${minutes}${t('sys.dashboard.uptime_minute')}`;
    return `${minutes}${t('sys.dashboard.uptime_minute')}`;
  }, [systemTime, t]);

  function tempColor(value: number | null | undefined, warn: number) {
    if (value == null) return 'text-muted-foreground';
    return value >= warn ? 'text-red-500' : 'text-emerald-500';
  }

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full w-full min-w-0 flex flex-col">
      {/* Header */}
      <h3 className="text-base font-bold text-foreground mb-3 flex items-center gap-2">
        <Activity className="w-4 h-4 text-primary" />
        {t('sys.dashboard.device_time', '设备状态')}
      </h3>

      <div className="space-y-2 flex-1">
        {/* Time */}
        <div className="text-center">
          <div className="text-xl font-mono font-semibold text-foreground tracking-tight">
            {formattedClock}
          </div>
        </div>

        {/* Uptime */}
        <div className="flex items-center justify-between px-1 pt-2 border-t border-border">
          <div className="flex items-center gap-1.5 text-muted-foreground">
            <Timer className="w-3.5 h-3.5" />
            <span className="text-sm">
              {t('sys.dashboard.uptime', '运行时间')}
            </span>
          </div>
          <span className="text-sm font-mono text-foreground">
            {uptimeDisplay}
          </span>
        </div>

        {/* Temperature Monitor */}
        <div className="px-1 pt-2 border-t border-border">
          <div className="flex items-center gap-1.5 mb-2">
            <Thermometer
              className={`w-3.5 h-3.5 ${hasWarning ? 'text-red-500' : 'text-emerald-500'}`}
            />
            <span className="text-sm text-muted-foreground">
              {t('sys.dashboard.temp_monitor', '温度监控')}
            </span>
          </div>
          <div className="grid grid-cols-2 gap-3">
            <TempCard
              label="SoC"
              value={socTemp}
              color={tempColor(socTemp, TEMP_WARN_SOC)}
            />
            <TempCard
              label={t('sys.dashboard.temp_board', 'Board')}
              value={temps?.board}
              color={tempColor(temps?.board, TEMP_WARN_BOARD)}
            />
          </div>
        </div>
      </div>
    </div>
  );
}

function TempCard({
  label,
  value,
  color,
}: {
  label: string;
  value: number | null | undefined;
  color: string;
}) {
  return (
    <div className="flex min-h-[76px] flex-col items-center justify-center gap-1 rounded-xl px-3 py-3 text-center">
      <span className="text-xs font-medium text-muted-foreground">{label}</span>
      <span
        className={`text-lg font-bold tabular-nums leading-none ${value == null ? 'text-muted-foreground' : color}`}
      >
        {value == null ? '--' : `${value.toFixed(1)}°`}
      </span>
    </div>
  );
}
