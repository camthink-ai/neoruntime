import i18n from '@/i18n/config';
import zhSys from '@/i18n/locales/zh/sys.json';
import enSys from '@/i18n/locales/en/sys.json';
import type { TFunction } from 'i18next';

const LOCALE_BUNDLES = {
  zh: zhSys,
  en: enSys,
} as const;

const ZH_COUNTRY_FALLBACK: Record<string, string> = {
  universal: '全球',
  china: '中国',
  japan: '日本',
  korea: '韩国',
  singapore: '新加坡',
  hong_kong: '中国香港',
  taiwan: '中国台湾',
  uae: '阿联酋',
  uk: '英国',
  france: '法国',
  germany: '德国',
  russia: '俄罗斯',
  usa: '美国',
  australia: '澳大利亚',
};

const COUNTRY_SLUG_BY_NAME: Record<string, string> = {
  Universal: 'universal',
  China: 'china',
  Japan: 'japan',
  Korea: 'korea',
  Singapore: 'singapore',
  'Hong Kong': 'hong_kong',
  Taiwan: 'taiwan',
  UAE: 'uae',
  UK: 'uk',
  France: 'france',
  Germany: 'germany',
  Russia: 'russia',
  USA: 'usa',
  Australia: 'australia',
};

/** Align with Settings → Time page: any non-English UI uses zh-CN. */
export function isChineseUiLanguage(language?: string): boolean {
  const lang = (language ?? i18n.language ?? i18n.resolvedLanguage ?? 'en')
    .trim()
    .toLowerCase()
    .replace(/_/g, '-');

  return lang !== 'en' && !lang.startsWith('en-');
}

function resolveCountrySlug(country: string): string | undefined {
  const trimmed = country.trim();
  if (COUNTRY_SLUG_BY_NAME[trimmed]) return COUNTRY_SLUG_BY_NAME[trimmed];

  const lower = trimmed.toLowerCase();
  return Object.entries(COUNTRY_SLUG_BY_NAME).find(
    ([name]) => name.toLowerCase() === lower
  )?.[1];
}

function getCountriesMap(lng: 'zh' | 'en'): Record<string, string> | undefined {
  const bundle = LOCALE_BUNDLES[lng];
  return bundle.sys?.time?.country as Record<string, string> | undefined;
}

function getCountryLabel(slug: string, language?: string): string | undefined {
  const lng = isChineseUiLanguage(language) ? 'zh' : 'en';
  const fromBundle = getCountriesMap(lng)?.[slug];
  if (fromBundle) return fromBundle;

  if (lng === 'zh') return ZH_COUNTRY_FALLBACK[slug];

  return undefined;
}

/** Use the same locale flag as Settings → Time date formatting. */
export function formatTimezoneCountryLabel(
  country: string,
  t: TFunction
): string {
  return translateTimezoneCountry(country, t);
}

export function translateTimezoneCountry(
  country: string,
  t?: TFunction,
  language?: string
): string {
  const trimmed = country?.trim();
  if (!trimmed) return country;

  const slug = resolveCountrySlug(trimmed);
  if (!slug) return trimmed;

  if (t) {
    return t(`sys.time.country.${slug}`, { defaultValue: trimmed });
  }

  return getCountryLabel(slug, language ?? i18n.language) ?? trimmed;
}
