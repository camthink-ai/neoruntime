import { useEffect, useMemo, useRef, useState } from 'react';
import { useQuery } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import timeApi from '@/services/api/time';

function formatDeviceTime(
  deviceNow: Date,
  locale: string,
  timeFormat: '12h' | '24h' | undefined,
  timezone: string | undefined
) {
  const is12h = (timeFormat ?? '24h') === '12h';
  try {
    return deviceNow.toLocaleTimeString(locale, {
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: is12h,
      timeZone: timezone,
    });
  } catch {
    return deviceNow.toLocaleTimeString(locale, {
      hour: '2-digit',
      minute: '2-digit',
      second: '2-digit',
      hour12: is12h,
    });
  }
}

/**
 * Live device clock aligned across dashboard and Settings → Time.
 * Uses React Query `dataUpdatedAt` as the client anchor so multiple
 * mounted consumers sharing cached `systemTime` stay in sync.
 */
export function useDeviceClock() {
  const { i18n } = useTranslation();
  const locale = i18n.language === 'en' ? 'en-US' : 'zh-CN';
  const [deviceNow, setDeviceNow] = useState<Date>(new Date());
  const baseServerUnixMsRef = useRef<number | null>(null);
  const receivedAtLocalMsRef = useRef<number | null>(null);

  const {
    data: systemTime,
    dataUpdatedAt,
    isLoading,
  } = useQuery({
    queryKey: ['systemTime'],
    queryFn: timeApi.getSystemTime,
    staleTime: 60000,
    refetchInterval: 60000,
    retry: false,
  });

  const { data: timeConfig } = useQuery({
    queryKey: ['timeConfig'],
    queryFn: timeApi.getTimeConfig,
    staleTime: 30000,
    refetchInterval: 30000,
    retry: false,
  });

  useEffect(() => {
    if (!systemTime?.unix_timestamp || !dataUpdatedAt) return;

    baseServerUnixMsRef.current = systemTime.unix_timestamp * 1000;
    receivedAtLocalMsRef.current = dataUpdatedAt;

    const tick = () => {
      const base = baseServerUnixMsRef.current;
      const receivedAt = receivedAtLocalMsRef.current;
      if (!base || !receivedAt) return;
      setDeviceNow(new Date(base + (Date.now() - receivedAt)));
    };

    tick();
    const timer = setInterval(tick, 1000);
    return () => clearInterval(timer);
  }, [systemTime?.unix_timestamp, dataUpdatedAt]);

  const formattedClock = useMemo(
    () => formatDeviceTime(
        deviceNow,
        locale,
        timeConfig?.time_format,
        timeConfig?.timezone
      ),
    [deviceNow, timeConfig?.time_format, timeConfig?.timezone, locale]
  );

  return {
    deviceNow,
    systemTime,
    timeConfig,
    formattedClock,
    formatDeviceTime: (timeFormat?: '12h' | '24h', timezone?: string) => formatDeviceTime(
        deviceNow,
        locale,
        timeFormat ?? timeConfig?.time_format,
        timezone ?? timeConfig?.timezone
      ),
    isLoading,
  };
}
