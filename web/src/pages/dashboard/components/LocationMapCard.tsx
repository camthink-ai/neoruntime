import { useQuery } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Crosshair, MapPin, Satellite } from 'lucide-react';
import {
  fetchCellularConfig,
  fetchCellularGnssStatus,
  fetchCellularStatus,
} from '@/services/settings';
import GnssMap from '@/pages/settings/network/components/GnssMap';
import { parseGnssFix } from '@/pages/settings/network/components/gnssGeo';
import { fetchGnssPlaceName } from '@/pages/settings/network/components/gnssPlace';

function dash(v: string | number | null | undefined): string {
  if (v == null || v === '') return '—';
  return String(v);
}

export default function LocationMapCard({ className }: { className?: string }) {
  const { t, i18n } = useTranslation();

  const { data: config } = useQuery({
    queryKey: ['cellularConfig'],
    queryFn: fetchCellularConfig,
    staleTime: 15_000,
  });
  const gnssOn = Boolean(config?.enabled && config?.gnss_enabled);

  const { data: gnss } = useQuery({
    queryKey: ['cellularGnssStatus'],
    queryFn: fetchCellularGnssStatus,
    enabled: gnssOn,
    refetchInterval: gnssOn ? 4000 : false,
    staleTime: 2000,
  });

  const fix = parseGnssFix(gnss?.latitude, gnss?.longitude);
  const placeBucket = fix ? `${fix.lat.toFixed(3)},${fix.lon.toFixed(3)}` : '';

  const { data: place } = useQuery({
    queryKey: ['cellularGnssPlace', placeBucket, i18n.language],
    queryFn: ({ signal }) => {
      if (!fix) throw new Error('no gnss fix');
      return fetchGnssPlaceName(fix.lat, fix.lon, i18n.language, signal);
    },
    enabled: Boolean(gnssOn && fix),
    staleTime: 60_000,
  });

  const fixLabel = (
    gnss?.fix === '3d'
      ? t('sys.cellular.gnss_fix_3d', '3D fix')
      : gnss?.fix === '2d'
        ? t('sys.cellular.gnss_fix_2d', '2D fix')
        : t('sys.cellular.gnss_fix_none', 'No fix')
  );

  return (
    <div
      className={`bg-card rounded-2xl p-5 shadow-sm border border-border h-full min-w-0 flex flex-col ${className || ''}`}
    >
      <h3 className="mb-3 flex items-center gap-2 text-base font-bold text-foreground">
        <MapPin className="h-4 w-4 text-primary" />
        {t('sys.dashboard.location', 'Location')}
      </h3>

      <div className="mb-2 grid grid-cols-3 gap-2 text-center">
        <div className="rounded-lg bg-muted/40 px-1.5 py-1.5">
          <div className="mb-0.5 flex items-center justify-center gap-1 text-[10px] text-muted-foreground">
            <Crosshair className="h-3 w-3" />
            {t('sys.cellular.gnss_fix', 'Fix')}
          </div>
          <div className="truncate text-xs font-semibold tabular-nums text-foreground">
            {gnssOn ? fixLabel : '—'}
          </div>
        </div>
        <div className="rounded-lg bg-muted/40 px-1.5 py-1.5">
          <div className="mb-0.5 flex items-center justify-center gap-1 text-[10px] text-muted-foreground">
            <Satellite className="h-3 w-3" />
            {t('sys.cellular.gnss_sats', 'Satellites')}
          </div>
          <div className="text-xs font-semibold tabular-nums text-foreground">
            {gnssOn ? dash(gnss?.satellites) : '—'}
          </div>
        </div>
        <div className="rounded-lg bg-muted/40 px-1.5 py-1.5">
          <div className="mb-0.5 text-[10px] text-muted-foreground">
            {t('sys.cellular.gnss_hdop', 'HDOP')}
          </div>
          <div className="text-xs font-semibold tabular-nums text-foreground">
            {gnssOn && gnss?.hdop != null ? gnss.hdop.toFixed(1) : '—'}
          </div>
        </div>
      </div>

      {(place?.trim() || (fix && gnssOn)) && (
        <p className="mb-1 line-clamp-2 text-[11px] leading-snug text-muted-foreground">
          {place?.trim()
            || (fix
              ? `${fix.lat.toFixed(5)}, ${fix.lon.toFixed(5)}`
              : '')}
        </p>
      )}

      <div className="min-h-0 flex-1">
        <GnssMap
          enabled={gnssOn}
          latitude={gnss?.latitude}
          longitude={gnss?.longitude}
          className="mt-0 h-full"
          mapClassName="h-[180px] lg:h-full lg:min-h-[180px]"
        />
      </div>
    </div>
  );
}

export function useCellularModulePresent() {
  return useQuery({
    queryKey: ['cellularStatus'],
    queryFn: fetchCellularStatus,
    staleTime: 15_000,
    refetchInterval: 30_000,
    select: s => Boolean(s.module_present && !s.stub),
  });
}
