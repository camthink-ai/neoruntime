/** Browser reverse-geocode via BigDataCloud client endpoint (CORS-friendly). */

type BigDataCloudAdmin = {
  name?: string;
  adminLevel?: number;
};

type BigDataCloudResp = {
  city?: string;
  locality?: string;
  principalSubdivision?: string;
  countryName?: string;
  localityInfo?: {
    administrative?: BigDataCloudAdmin[];
  };
};

function placeLang(uiLang: string): string {
  return uiLang.toLowerCase().startsWith('zh') ? 'zh-CN' : 'en';
}

function formatPlace(d: BigDataCloudResp): string {
  const admins = [...(d.localityInfo?.administrative ?? [])]
    .filter(a => {
      const level = a.adminLevel ?? 0;
      return level >= 4 && level <= 8 && (a.name ?? '').trim();
    })
    .sort((a, b) => (a.adminLevel ?? 0) - (b.adminLevel ?? 0));

  const fromAdmin: string[] = [];
  const seen = new Set<string>();
  for (const a of admins) {
    const name = (a.name ?? '').trim();
    if (seen.has(name)) continue;
    seen.add(name);
    fromAdmin.push(name);
    if (fromAdmin.length === 4) break;
  }
  if (fromAdmin.length > 0) return fromAdmin.join(' · ');

  const parts = [d.principalSubdivision, d.city, d.locality]
    .map(s => (s ?? '').trim())
    .filter(Boolean)
    .filter((s, i, arr) => i === 0 || s !== arr[i - 1]);
  if (parts.length > 0) return parts.join(' · ');
  return (d.countryName ?? '').trim();
}

export async function fetchGnssPlaceName(
  lat: number,
  lon: number,
  uiLang: string,
  signal?: AbortSignal
): Promise<string> {
  const q = new URLSearchParams({
    latitude: lat.toFixed(6),
    longitude: lon.toFixed(6),
    localityLanguage: placeLang(uiLang),
  });
  const resp = await fetch(
    `https://api.bigdatacloud.net/data/reverse-geocode-client?${q}`,
    { signal, headers: { Accept: 'application/json' } }
  );
  if (!resp.ok) throw new Error(`geocode http ${resp.status}`);
  const name = formatPlace((await resp.json()) as BigDataCloudResp);
  if (!name) throw new Error('empty place');
  return name;
}
