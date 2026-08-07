import { useState, useEffect, useMemo, useRef } from 'react';
import { useQuery, useQueryClient, useMutation } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { TimezoneSearchSelect } from '@/components/TimezoneSearchSelect';
import { Label } from '@/components/ui/label';

import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Input } from '@/components/ui/input';
import { Separator } from '@/components/ui/separator';
import { RadioGroup, RadioGroupItem } from '@/components/ui/radio-group';
import { Card, CardContent } from '@/components/ui/card';
import { DateTimePicker } from '@/components/ui/datetime-picker';
import { Clock, Globe, RefreshCw, Save } from 'lucide-react';
import { toast } from 'sonner';
import { cn } from '@/lib/utils';
import { TimeSettingsSkeleton } from './components/TimeSettingsSkeleton';
import timeApi from '@/services/api/time';
import type { SaveTimeConfigRequest } from '@/services/api/time';
import { useDeviceClock } from '@/hooks/useDeviceClock';
import {
  formatManualDateTimeRFC3339,
  toDeviceWallClockDate,
} from './utils/manualDateTime';
import type { TimezoneData } from '@/services/api/time';
import {
  formatTimezoneCountryLabel,
  isChineseUiLanguage,
} from './utils/timezoneCountry';
import { shouldAutoSyncFromClient } from './utils/autoSync';

const UTC_TIMEZONE_FALLBACK: TimezoneData = {
  name: 'UTC',
  country: 'Universal',
  offset: 'UTC+00:00',
  offset_sec: 0,
};

type TimeFormat = '12h' | '24h';
type SyncMode = 'ntp' | 'manual';

const SYNC_INTERVALS = [
  { labelKey: 'sys.time.interval_1m', value: 60 },
  { labelKey: 'sys.time.interval_5m', value: 300 },
  { labelKey: 'sys.time.interval_15m', value: 900 },
  { labelKey: 'sys.time.interval_30m', value: 1800 },
  { labelKey: 'sys.time.interval_1h', value: 3600 },
  { labelKey: 'sys.time.interval_6h', value: 21600 },
  { labelKey: 'sys.time.interval_12h', value: 43200 },
  { labelKey: 'sys.time.interval_24h', value: 86400 },
];

interface FormState {
  timezone: string;
  timeFormat: TimeFormat;
  syncMode: SyncMode;
  ntpServer: string;
  ntpInterval: number;
  manualDateTime: string; // RFC3339, e.g. 2026-04-21T12:34:56.000Z
}

export default function TimeSettings() {
  const { t, i18n } = useTranslation();
  const queryClient = useQueryClient();
  const locale = isChineseUiLanguage(i18n.language) ? 'zh-CN' : 'en-US';

  const { deviceNow, systemTime, timeConfig, formatDeviceTime, isLoading } =    useDeviceClock();

  const { data: timezones = [] } = useQuery({
    queryKey: ['timezones'],
    queryFn: timeApi.getTimezones,
    staleTime: Infinity,
    retry: false,
  });

  // Form state
  const [form, setForm] = useState<FormState>({
    timezone: '',
    timeFormat: '24h',
    syncMode: 'ntp',
    ntpServer: 'pool.ntp.org',
    ntpInterval: 3600,
    manualDateTime: '',
  });

  // Track if form has been initialized from API
  const [initialized, setInitialized] = useState(false);

  // Auto-sync from browser if device time is significantly off
  const autoSyncRef = useRef(false);

  // Tracks whether the user has touched the manual "Set Time" picker, so the
  // auto-seed effect below never overwrites an explicit choice (including a
  // deliberate clear). Resets on full page reload, so echo still works on refresh.
  const userEditedManualRef = useRef(false);
  useEffect(() => {
    if (!systemTime?.unix_timestamp || !timeConfig || autoSyncRef.current) {
      return;
    }

    if (
      shouldAutoSyncFromClient({
        deviceUnixTimestamp: systemTime.unix_timestamp,
        browserUnixTimestamp: Math.floor(Date.now() / 1000),
        persistedSyncMode: timeConfig.sync_mode,
        formSyncMode: form.syncMode,
        autoSync: timeConfig.auto_sync,
      })
    ) {
      autoSyncRef.current = true;
      timeApi
        .syncFromClient(Date.now())
        .then(result => {
          if (result.synced) {
            queryClient.invalidateQueries({ queryKey: ['systemTime'] });
            queryClient.invalidateQueries({ queryKey: ['timeConfig'] });
          }
        })
        .catch(() => {
          // Silent fail — don't disrupt user experience
        });
    }
  }, [form.syncMode, queryClient, systemTime?.unix_timestamp, timeConfig]);

  // Initialize form from API data
  useEffect(() => {
    if (timeConfig && !initialized) {
      const rawSyncMode = timeConfig.sync_mode;
      const syncMode: SyncMode = rawSyncMode === 'manual' ? 'manual' : 'ntp';

      setForm({
        timezone: timeConfig.timezone || '',
        timeFormat: timeConfig.time_format || '24h',
        syncMode,
        ntpServer: timeConfig.ntp?.server || 'pool.ntp.org',
        ntpInterval: timeConfig.ntp?.interval || 3600,
        manualDateTime: '',
      });
      setInitialized(true);
    }
  }, [timeConfig, initialized]);

  // Echo the live device time into the manual "Set Time" field so it isn't
  // blank on page load/refresh (or when first switching to Manual mode). The
  // picker is a wall-clock editor: onChange stamps the chosen numbers with the
  // device-TZ offset via formatManualDateTimeRFC3339. To keep display↔save
  // consistent we seed a Date whose LOCAL wall clock reads the device's current
  // wall clock (toDeviceWallClockDate), then run it through the same formatter.
  // One-shot per user-edit cycle: once the user touches the picker we stop
  // auto-filling, and an already-populated field is never re-seeded.
  useEffect(() => {
    if (userEditedManualRef.current) return;
    if (form.syncMode !== 'manual') return;
    if (form.manualDateTime) return;
    if (!form.timezone) return;
    if (!systemTime?.current_time) return;

    const instant = new Date(systemTime.current_time);
    if (Number.isNaN(instant.getTime())) return;
    const wallClock = toDeviceWallClockDate(instant, form.timezone);
    setForm(prev => ({
      ...prev,
      manualDateTime: formatManualDateTimeRFC3339(wallClock, prev.timezone),
    }));
  }, [
    form.syncMode,
    form.manualDateTime,
    form.timezone,
    systemTime?.current_time,
  ]);

  // Update form helpers
  const updateForm = <K extends keyof FormState>(
    key: K,
    value: FormState[K]
  ) => {
    setForm(prev => ({ ...prev, [key]: value }));
  };

  const getErrorMessage = (err: any) => {
    const detail = err?.response?.data?.detail || err?.response?.data?.message;
    if (typeof detail === 'string') return detail;
    if (err?.message) return err.message;
    return t('common.save_failed', 'Save Failed');
  };

  // NTP sync mutation (separate from save)
  const syncNTPMutation = useMutation({
    mutationFn: () => timeApi.syncNTP(),
    onSuccess: () => {
      toast.success(t('sys.time.ntp_sync_triggered', 'NTP 同步已触发'));
      setTimeout(() => {
        queryClient.invalidateQueries({ queryKey: ['timeConfig'] });
      }, 2000);
    },
    onError: err => {
      toast.error(getErrorMessage(err));
    },
  });

  // Save all config
  const saveMutation = useMutation({
    mutationFn: async () => {
      const data: SaveTimeConfigRequest = {
        timezone: form.timezone,
        time_format: form.timeFormat,
        sync_mode: form.syncMode,
      };

      if (form.syncMode === 'ntp') {
        data.ntp_server = form.ntpServer;
        data.ntp_interval = form.ntpInterval;
      }

      if (form.syncMode === 'manual' && form.manualDateTime) {
        data.manual_datetime = form.manualDateTime;
      }

      await timeApi.saveTimeConfig(data);
    },
    onSuccess: () => {
      toast.success(t('common.saved', 'Saved'));
      queryClient.invalidateQueries({ queryKey: ['timeConfig'] });
      queryClient.invalidateQueries({ queryKey: ['systemTime'] });
    },
    onError: err => {
      toast.error(getErrorMessage(err));
    },
  });

  // Computed values
  const currentTimezoneDisplay = useMemo(() => {
    if (!timeConfig) return '-';
    const tz =      timezones.find(tzItem => tzItem.name === timeConfig.timezone)
      ?? (timeConfig.timezone === 'UTC' ? UTC_TIMEZONE_FALLBACK : undefined);
    if (!tz) return timeConfig.timezone;
    const countryLabel = formatTimezoneCountryLabel(tz.country, t);
    return `${tz.name} (${tz.offset}, ${countryLabel})`;
  }, [timeConfig, timezones, locale, t, i18n.language]);

  const formattedClock = useMemo(
    () => formatDeviceTime(form.timeFormat, timeConfig?.timezone),
    [formatDeviceTime, form.timeFormat, timeConfig?.timezone, deviceNow]
  );

  const formattedDate = useMemo(() => {
    const d = deviceNow;
    const options: Intl.DateTimeFormatOptions =      locale === 'zh-CN'
        ? { year: 'numeric', month: 'long', day: 'numeric' }
        : { year: 'numeric', month: 'short', day: '2-digit' };
    try {
      return d.toLocaleDateString(locale, {
        ...options,
        timeZone: timeConfig?.timezone,
      });
    } catch {
      return d.toLocaleDateString(locale, options);
    }
  }, [deviceNow, timeConfig?.timezone, locale]);

  const formattedWeekday = useMemo(() => {
    const d = deviceNow;
    try {
      return d.toLocaleDateString(locale, {
        weekday: 'long',
        timeZone: timeConfig?.timezone,
      });
    } catch {
      return d.toLocaleDateString(locale, { weekday: 'long' });
    }
  }, [deviceNow, timeConfig?.timezone, locale]);

  const syncModes: { value: SyncMode; label: string }[] = [
    { value: 'ntp', label: t('sys.time.sync_ntp', 'NTP 同步') },
    { value: 'manual', label: t('sys.time.sync_manual', 'Manual') },
  ];

  const handleSave = () => {
    if (form.syncMode === 'manual' && !form.manualDateTime) {
      toast.error(
        t('sys.time.invalid_datetime', 'Please enter a valid datetime')
      );
      return;
    }
    saveMutation.mutate();
  };

  if (isLoading) {
    return <TimeSettingsSkeleton />;
  }

  return (
    <div className="p-6 md:p-12 max-w-4xl w-full min-h-screen bg-background mx-auto">
      <Card>
        <CardContent className="p-6 md:p-8 space-y-6">
          {/* Section 1: Current System Time */}
          <section>
            <div className="flex flex-col items-start gap-4 md:flex-row md:items-start md:justify-between md:gap-6">
              {/* 左侧：时间 + 日期 */}
              <div className="min-w-0 w-full md:w-auto">
                <div className="font-mono text-4xl md:text-5xl font-semibold text-foreground tracking-tight tabular-nums leading-none">
                  {formattedClock}
                </div>
                <div className="mt-2 text-sm text-muted-foreground">
                  <span>{formattedDate}</span>
                  <span className="mx-2 text-muted-foreground/40">·</span>
                  <span>{formattedWeekday}</span>
                </div>
              </div>

              {/* 右侧：时区 / 区域 */}
              <div className="flex w-full items-center gap-2 md:my-auto md:w-auto md:shrink-0 md:gap-10">
                <div className="flex items-center gap-2">
                  <Clock className="w-4 h-4 text-muted-foreground" />
                  <div className="leading-tight">
                    <div className="text-xs text-muted-foreground">
                      {t('sys.time.timezone', 'Timezone')}
                    </div>
                    <div className="text-sm text-foreground">
                      {currentTimezoneDisplay}
                    </div>
                  </div>
                </div>

                {/* <div className="flex items-center gap-2">
                  <Globe className="w-4 h-4 text-muted-foreground" />
                  <div className="leading-tight">
                    <div className="text-xs text-muted-foreground">
                      {t('sys.time.region', '区域')}
                    </div>
                    <div className="text-sm font-semibold text-foreground">
                      {regionDisplay}
                    </div>
                  </div>
                </div> */}
              </div>
            </div>
          </section>

          <Separator />

          {/* Section 2: System Time Settings */}
          <section>
            <div className="flex items-center gap-2 text-foreground font-semibold text-base">
              <Globe className="w-4.5 h-4.5" strokeWidth={2.5} />
              {t('sys.time.time_settings', 'System Time Settings')}
            </div>

            <div className="space-y-6 pt-4">
              {/* Timezone */}
              <div className="space-y-2">
                <Label className="text-sm font-medium">
                  {t('sys.time.timezone', 'Timezone')}
                </Label>
                <TimezoneSearchSelect
                  timezones={timezones}
                  value={form.timezone}
                  onChange={v => updateForm('timezone', v)}
                  placeholder={t('sys.time.select_timezone', 'Select Timezone')}
                  t={t}
                />
              </div>

              {/* <Separator /> */}

              {/* Time Format */}
              <div className="space-y-2">
                <Label className="text-sm font-medium">
                  {t('sys.time.time_format', 'Time Format')}
                </Label>
                <Select
                  value={form.timeFormat}
                  onValueChange={v => updateForm('timeFormat', v as TimeFormat)}
                >
                  <SelectTrigger className="max-w-md">
                    <SelectValue />
                  </SelectTrigger>
                  <SelectContent>
                    <SelectItem value="24h">
                      {t('sys.time.format_24h', '24 小时制')}
                    </SelectItem>
                    <SelectItem value="12h">
                      {t('sys.time.format_12h', '12 小时制')}
                    </SelectItem>
                  </SelectContent>
                </Select>
              </div>

              {/* Sync Mode */}
              <div className="space-y-2">
                <Label className="text-sm font-medium">
                  {t('sys.time.sync_mode', 'Sync Mode')}
                </Label>

                <RadioGroup
                  value={form.syncMode}
                  onValueChange={v => updateForm('syncMode', v as SyncMode)}
                  className="flex flex-wrap gap-6"
                >
                  {syncModes.map(mode => (
                    <label
                      key={mode.value}
                      className="flex items-center gap-2 cursor-pointer select-none"
                    >
                      <RadioGroupItem
                        value={mode.value}
                        aria-label={mode.label}
                        className="mt-px"
                      />
                      <span className="text-sm font-medium text-foreground">
                        {mode.label}
                      </span>
                    </label>
                  ))}
                </RadioGroup>

                {/* NTP Mode Settings */}
                {form.syncMode === 'ntp' && (
                  <div className="space-y-4 pt-2">
                    {/* NTP Server */}
                    <div className="space-y-2">
                      <Label className="text-sm text-muted-foreground">
                        {t('sys.time.ntp_server', 'NTP 服务器')}
                      </Label>
                      <div className="flex items-center gap-3">
                        <Input
                          value={form.ntpServer}
                          onChange={e => updateForm('ntpServer', e.target.value)}
                          className="max-w-md"
                          placeholder="pool.ntp.org"
                        />
                        <Button
                          variant="outline"
                          size="sm"
                          onClick={() => syncNTPMutation.mutate()}
                          disabled={syncNTPMutation.isPending}
                        >
                          <RefreshCw
                            className={cn(
                              'w-4 h-4 mr-2',
                              syncNTPMutation.isPending && 'animate-spin'
                            )}
                          />
                          {t('sys.time.sync_now', 'Sync Now')}
                        </Button>
                      </div>
                    </div>

                    {/* Sync Interval */}
                    <div className="space-y-2">
                      <Label className="text-sm text-muted-foreground">
                        {t('sys.time.sync_interval', 'Sync Interval')}
                      </Label>
                      <Select
                        value={String(form.ntpInterval)}
                        onValueChange={v => updateForm('ntpInterval', Number(v))}
                      >
                        <SelectTrigger className="max-w-md">
                          <SelectValue />
                        </SelectTrigger>
                        <SelectContent>
                          {SYNC_INTERVALS.map(opt => (
                            <SelectItem
                              key={opt.value}
                              value={String(opt.value)}
                            >
                              {t(opt.labelKey)}
                            </SelectItem>
                          ))}
                        </SelectContent>
                      </Select>
                    </div>
                  </div>
                )}

                {/* Manual Mode Settings */}
                {form.syncMode === 'manual' && (
                  <div className="space-y-3 pt-2">
                    <div className="space-y-2">
                      <Label className="text-sm text-foreground">
                        {t('sys.time.set_time_manually', 'Set Time')}
                      </Label>
                      <div className="flex items-center gap-3">
                        <DateTimePicker
                          value={
                            form.manualDateTime
                              ? new Date(form.manualDateTime)
                              : undefined
                          }
                          onChange={date => {
                            userEditedManualRef.current = true;
                            if (date && form.timezone) {
                              updateForm(
                                'manualDateTime',
                                formatManualDateTimeRFC3339(date, form.timezone)
                              );
                            } else if (!date) {
                              updateForm('manualDateTime', '');
                            }
                          }}
                          className="max-w-xs"
                        />
                      </div>
                    </div>
                  </div>
                )}
              </div>

              <Separator />

              {/* Save Button */}
              <div className="flex justify-end pt-2">
                <Button
                  onClick={handleSave}
                  disabled={saveMutation.isPending}
                  variant="carbon"
                >
                  <Save className="w-4 h-4 mr-2" />
                  {saveMutation.isPending
                    ? t('common.saving', 'Saving...')
                    : t('common.save', 'Save')}
                </Button>
              </div>
            </div>
          </section>
        </CardContent>
      </Card>
    </div>
  );
}
