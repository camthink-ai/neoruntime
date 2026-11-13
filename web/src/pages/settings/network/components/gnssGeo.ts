/** Shared GNSS coordinate helpers (WGS84 from modem). */

export type GnssFix = { lat: number; lon: number };

export function parseGnssFix(
  lat?: number | null,
  lon?: number | null
): GnssFix | null {
  if (
    typeof lat !== 'number'
    || typeof lon !== 'number'
    || !Number.isFinite(lat)
    || !Number.isFinite(lon)
    || Math.abs(lat) > 90
    || Math.abs(lon) > 180
  ) {
    return null;
  }
  return { lat, lon };
}

function outOfChina(lat: number, lon: number): boolean {
  return lon < 72.004 || lon > 137.8347 || lat < 0.8293 || lat > 55.8271;
}

function transformLat(x: number, y: number): number {
  let ret =    -100
    + 2 * x
    + 3 * y
    + 0.2 * y * y
    + 0.1 * x * y
    + 0.2 * Math.sqrt(Math.abs(x));
  ret
    += ((20 * Math.sin(6 * x * Math.PI) + 20 * Math.sin(2 * x * Math.PI)) * 2)
    / 3;
  ret
    += ((20 * Math.sin(y * Math.PI) + 40 * Math.sin((y / 3) * Math.PI)) * 2) / 3;
  ret
    += ((160 * Math.sin((y / 12) * Math.PI) + 320 * Math.sin((y * Math.PI) / 30))
      * 2)
    / 3;
  return ret;
}

function transformLon(x: number, y: number): number {
  let ret =    300
    + x
    + 2 * y
    + 0.1 * x * x
    + 0.1 * x * y
    + 0.1 * Math.sqrt(Math.abs(x));
  ret
    += ((20 * Math.sin(6 * x * Math.PI) + 20 * Math.sin(2 * x * Math.PI)) * 2)
    / 3;
  ret
    += ((20 * Math.sin(x * Math.PI) + 40 * Math.sin((x / 3) * Math.PI)) * 2) / 3;
  ret
    += ((150 * Math.sin((x / 12) * Math.PI) + 300 * Math.sin((x / 30) * Math.PI))
      * 2)
    / 3;
  return ret;
}

/** Modem fix is WGS84; Gaode tiles are GCJ-02. */
export function wgs84ToGcj02(lat: number, lon: number): [number, number] {
  if (outOfChina(lat, lon)) return [lat, lon];
  let dLat = transformLat(lon - 105, lat - 35);
  let dLon = transformLon(lon - 105, lat - 35);
  const radLat = (lat / 180) * Math.PI;
  let magic = Math.sin(radLat);
  magic = 1 - 0.006693421622965943 * magic * magic;
  const sqrtMagic = Math.sqrt(magic);
  dLat =    (dLat * 180)
    / (((6378245 * (1 - 0.006693421622965943)) / (magic * sqrtMagic)) * Math.PI);
  dLon = (dLon * 180) / ((6378245 / sqrtMagic) * Math.cos(radLat) * Math.PI);
  return [lat + dLat, lon + dLon];
}
