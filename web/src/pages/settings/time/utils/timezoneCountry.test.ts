import { describe, expect, it } from 'vitest';
import {
  isChineseUiLanguage,
  formatTimezoneCountryLabel,
  translateTimezoneCountry,
} from '@/pages/settings/time/utils/timezoneCountry';
import zhSys from '@/i18n/locales/zh/sys.json';

const zhT = ((key: string, options?: { defaultValue?: string }) => {
  const slug = key.replace('sys.time.country.', '');
  return (
    (zhSys.sys?.time?.country as Record<string, string> | undefined)?.[slug]
    ?? options?.defaultValue
    ?? key
  );
}) as never;

describe('timezoneCountry', () => {
  it('reads 全球 from zh sys.json bundle path', () => {
    expect(zhSys.sys?.time?.country?.universal).toBe('全球');
  });

  it('formatTimezoneCountryLabel maps Universal to 全球 via t()', () => {
    expect(formatTimezoneCountryLabel('Universal', zhT)).toBe('全球');
    expect(formatTimezoneCountryLabel('universal', zhT)).toBe('全球');
  });

  it('translateTimezoneCountry keeps Universal for English t()', () => {
    const enT = ((key: string, options?: { defaultValue?: string }) => options?.defaultValue ?? key) as never;
    expect(translateTimezoneCountry('Universal', enT)).toBe('Universal');
  });

  it('detects Chinese UI language consistently with the time page', () => {
    expect(isChineseUiLanguage('zh')).toBe(true);
    expect(isChineseUiLanguage('zh-CN')).toBe(true);
    expect(isChineseUiLanguage('en')).toBe(false);
    expect(isChineseUiLanguage('en-US')).toBe(false);
  });
});
