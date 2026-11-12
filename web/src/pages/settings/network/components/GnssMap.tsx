import { useEffect, useRef } from 'react';
import L from 'leaflet';
import 'leaflet/dist/leaflet.css';
import { useTranslation } from 'react-i18next';
import { cn } from '@/lib/utils';
import { parseGnssFix, wgs84ToGcj02, type GnssFix } from './gnssGeo';

const DEFAULT_CENTER: L.LatLngExpression = [24.48, 118.09];
const DEFAULT_ZOOM = 11;
const FIX_ZOOM = 16;
const TILE_ERROR_SWITCH = 4;
/** Ignore tiny GNSS jitter when re-centering (~15 m). */
const RECENTER_EPS_DEG = 0.00015;

const MARKER_STYLE: L.CircleMarkerOptions = {
  radius: 8,
  color: '#059669',
  weight: 2,
  fillColor: '#10b981',
  fillOpacity: 0.9,
};

export type GnssMapProps = {
  enabled: boolean;
  /** WGS84 from modem */
  latitude?: number | null;
  longitude?: number | null;
  className?: string;
  mapClassName?: string;
};

type BasemapCrs = 'wgs84' | 'gcj02';

type BasemapProvider = {
  crs: BasemapCrs;
  url: string;
  options: L.TileLayerOptions;
};

/** Prefer Gaode (GCJ-02) in CN; fall back to Esri / Carto (WGS84). */
const BASEMAPS: BasemapProvider[] = [
  {
    crs: 'gcj02',
    url: 'https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}',
    options: {
      subdomains: '1234',
      maxZoom: 18,
      attribution: '&copy; <a href="https://lbs.amap.com/">AMap</a>',
    },
  },
  {
    crs: 'wgs84',
    url: 'https://server.arcgisonline.com/ArcGIS/rest/services/World_Street_Map/MapServer/tile/{z}/{y}/{x}',
    options: {
      maxZoom: 19,
      attribution: 'Tiles &copy; <a href="https://www.esri.com/">Esri</a>',
    },
  },
  {
    crs: 'wgs84',
    url: 'https://{s}.basemaps.cartocdn.com/rastertiles/voyager/{z}/{x}/{y}.png',
    options: {
      subdomains: 'abcd',
      maxZoom: 20,
      attribution:
        '&copy; <a href="https://www.openstreetmap.org/copyright">OSM</a> &copy; <a href="https://carto.com/">CARTO</a>',
    },
  },
];

type MapRuntime = {
  layer: L.TileLayer | null;
  providerIndex: number;
  crs: BasemapCrs;
  tileErrors: number;
  viewCommitted: boolean;
};

function displayLatLng(fix: GnssFix, crs: BasemapCrs): L.LatLngExpression {
  return crs === 'gcj02' ? wgs84ToGcj02(fix.lat, fix.lon) : [fix.lat, fix.lon];
}

function movedEnough(a: GnssFix, b: GnssFix): boolean {
  return (
    Math.abs(a.lat - b.lat) > RECENTER_EPS_DEG
    || Math.abs(a.lon - b.lon) > RECENTER_EPS_DEG
  );
}

function attachBasemap(
  map: L.Map,
  providerIndex: number,
  runtime: MapRuntime,
  onCrsChange: (crs: BasemapCrs) => void
): L.TileLayer {
  const provider = BASEMAPS[providerIndex] ?? BASEMAPS[0];
  const layer = L.tileLayer(provider.url, provider.options);
  runtime.layer = layer;
  runtime.providerIndex = providerIndex;
  runtime.crs = provider.crs;
  runtime.tileErrors = 0;

  layer.on('tileerror', () => {
    runtime.tileErrors += 1;
    if (runtime.tileErrors < TILE_ERROR_SWITCH) return;
    const next = runtime.providerIndex + 1;
    if (next >= BASEMAPS.length) return;
    if (runtime.layer) map.removeLayer(runtime.layer);
    attachBasemap(map, next, runtime, onCrsChange).addTo(map);
    onCrsChange(runtime.crs);
  });
  layer.on('tileload', () => {
    runtime.tileErrors = 0;
  });
  return layer;
}

export default function GnssMap({
  enabled,
  latitude,
  longitude,
  className,
  mapClassName,
}: GnssMapProps) {
  const { t } = useTranslation();
  const containerRef = useRef<HTMLDivElement>(null);
  const mapRef = useRef<L.Map | null>(null);
  const markerRef = useRef<L.CircleMarker | null>(null);
  const runtimeRef = useRef<MapRuntime | null>(null);
  const fixRef = useRef<GnssFix | null>(null);
  const fixed = Boolean(parseGnssFix(latitude, longitude));

  const syncMarker = (crs: BasemapCrs, animate: boolean) => {
    const map = mapRef.current;
    const runtime = runtimeRef.current;
    const fix = fixRef.current;
    if (!map || !runtime || !enabled || !fix) {
      markerRef.current?.remove();
      markerRef.current = null;
      return;
    }

    if (!runtime.layer) {
      attachBasemap(map, 0, runtime, nextCrs => syncMarker(nextCrs, false)).addTo(
        map
      );
    }

    const pos = displayLatLng(fix, crs);
    if (!markerRef.current) {
      markerRef.current = L.circleMarker(pos, MARKER_STYLE).addTo(map);
    } else {
      markerRef.current.setLatLng(pos);
    }

    const first = !runtime.viewCommitted;
    const center = map.getCenter();
    const ll = L.latLng(pos);
    const shouldRecenter =      first
      || map.getZoom() !== FIX_ZOOM
      || (animate
        && movedEnough(
          { lat: center.lat, lon: center.lng },
          { lat: ll.lat, lon: ll.lng }
        ));

    if (shouldRecenter) {
      map.setView(pos, FIX_ZOOM, { animate: animate && !first });
      runtime.viewCommitted = true;
    }
    map.invalidateSize();
  };

  useEffect(() => {
    const el = containerRef.current;
    if (!el || mapRef.current) return;

    const startFix = parseGnssFix(latitude, longitude);
    const startCrs = BASEMAPS[0].crs;
    const startCenter = startFix
      ? displayLatLng(startFix, startCrs)
      : DEFAULT_CENTER;

    const map = L.map(el, {
      zoomControl: true,
      attributionControl: true,
      dragging: true,
      scrollWheelZoom: false,
      fadeAnimation: false,
      zoomAnimation: false,
    }).setView(startCenter, startFix ? FIX_ZOOM : DEFAULT_ZOOM);

    const runtime: MapRuntime = {
      layer: null,
      providerIndex: 0,
      crs: startCrs,
      tileErrors: 0,
      viewCommitted: Boolean(startFix),
    };
    runtimeRef.current = runtime;
    mapRef.current = map;

    // Defer basemap until first fix so refresh does not flash the default city.
    if (startFix) {
      fixRef.current = startFix;
      attachBasemap(map, 0, runtime, crs => syncMarker(crs, false)).addTo(map);
      markerRef.current = L.circleMarker(
        displayLatLng(startFix, startCrs),
        MARKER_STYLE
      ).addTo(map);
    }

    const ro = new ResizeObserver(() => map.invalidateSize());
    ro.observe(el);
    const t1 = window.setTimeout(() => map.invalidateSize(), 50);
    const t2 = window.setTimeout(() => map.invalidateSize(), 300);

    return () => {
      window.clearTimeout(t1);
      window.clearTimeout(t2);
      ro.disconnect();
      map.remove();
      mapRef.current = null;
      markerRef.current = null;
      runtimeRef.current = null;
      fixRef.current = null;
    };
  }, []);

  useEffect(() => {
    const map = mapRef.current;
    const runtime = runtimeRef.current;
    if (!map || !runtime) return;

    const next = parseGnssFix(latitude, longitude);
    if (!enabled || !next) {
      const prev = fixRef.current;
      fixRef.current = null;
      markerRef.current?.remove();
      markerRef.current = null;
      if (!prev && !runtime.viewCommitted && !runtime.layer) {
        map.setView(DEFAULT_CENTER, DEFAULT_ZOOM, { animate: false });
      }
      map.invalidateSize();
      return;
    }

    const prev = fixRef.current;
    const first = !runtime.viewCommitted;
    fixRef.current = next;

    if (!first && prev && !movedEnough(prev, next) && markerRef.current) {
      markerRef.current.setLatLng(displayLatLng(next, runtime.crs));
      return;
    }

    syncMarker(runtime.crs, !first && Boolean(prev));
  }, [enabled, fixed, latitude, longitude]);

  const overlay = !enabled
    ? t('sys.cellular.gnss_map_off', 'Turn on GNSS to show position')
    : !fixed
      ? t('sys.cellular.gnss_map_acquiring', 'Waiting for GNSS fix…')
      : null;

  return (
    <div
      className={cn(
        'relative mt-3 overflow-hidden rounded-lg border border-border',
        className
      )}
    >
      {/* Visual classes stay on the wrapper: Leaflet mutates the host classList. */}
      <div
        className={cn(
          'h-[220px] w-full bg-muted/40',
          mapClassName,
          (!enabled || !fixed) && 'grayscale-[0.35]'
        )}
      >
        <div ref={containerRef} className="h-full w-full" />
      </div>
      {overlay ? (
        <div className="pointer-events-none absolute inset-0 flex items-center justify-center bg-background/55 px-4 text-center">
          <span className="rounded-md bg-background/90 px-3 py-1.5 text-xs font-medium text-muted-foreground shadow-sm">
            {overlay}
          </span>
        </div>
      ) : null}
    </div>
  );
}
