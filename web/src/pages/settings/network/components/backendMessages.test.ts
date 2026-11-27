import { describe, expect, it } from 'vitest';
import zhSys from '@/i18n/locales/zh/sys.json';
import enSys from '@/i18n/locales/en/sys.json';
import {
  EXACT_MESSAGES,
  PREFIX_MESSAGES,
  localizeBackendMessage,
  regStateLabel,
} from './backendMessages';

/** Pass-through translator: en locale behavior (fallback = the English text). */
const t = (key: string, fallback: string) => `[[${key}]]${fallback}`;

/** Resolve keys against a real locale resource, falling back like i18next. */
const makeT = (resource: Record<string, unknown>) => (key: string, fallback: string) => {
    const val = key.split('.').reduce<unknown>(
      (o, k) => (o && typeof o === 'object' ? (o as Record<string, unknown>)[k] : undefined),
      resource
    );
    return typeof val === 'string' ? val : fallback;
  };

const tZh = makeT(zhSys);
const tEn = makeT(enSys);

describe('localizeBackendMessage', () => {
  it('maps known status messages to their i18n keys', () => {
    expect(localizeBackendMessage('Module power is off.', t)).toBe(
      '[[sys.cellular.msg_power_off]]Module power is off.'
    );
  });

  it('keeps the dynamic suffix of prefix-matched messages', () => {
    expect(
      localizeBackendMessage('Failed to power module: gpio chip busy', t)
    ).toBe('[[sys.cellular.msg_power_fail]]Failed to power module: gpio chip busy');
  });

  it('passes unknown messages through untouched', () => {
    expect(localizeBackendMessage('something unexpected', t)).toBe(
      'something unexpected'
    );
  });

  it('returns empty for null, undefined, and blank input', () => {
    expect(localizeBackendMessage(null, t)).toBe('');
    expect(localizeBackendMessage(undefined, t)).toBe('');
    expect(localizeBackendMessage('   ', t)).toBe('');
  });
});

describe('localizeBackendMessage with real locales', () => {
  it('translates the "ping failed" toast in zh and en', () => {
    expect(localizeBackendMessage('ping failed', tZh)).toBe('ping 失败');
    expect(localizeBackendMessage('ping failed', tEn)).toBe('ping failed');
  });

  it('translates the composed PLMN validation toast in zh and en', () => {
    const raw = 'failed to save cellular config: plmn must be 5–6 digits (MCC+MNC)';
    expect(localizeBackendMessage(raw, tZh)).toBe(
      '保存蜂窝配置失败：PLMN 必须为 5–6 位数字（MCC+MNC）'
    );
    expect(localizeBackendMessage(raw, tEn)).toBe(
      'Failed to save cellular config: plmn must be 5–6 digits (MCC+MNC)'
    );
  });
});

describe('locale parity', () => {
  it('keeps zh and en cellular/network key sets identical', () => {
    expect(Object.keys(zhSys.sys.cellular).sort()).toEqual(
      Object.keys(enSys.sys.cellular).sort()
    );
    expect(Object.keys(zhSys.sys.network).sort()).toEqual(
      Object.keys(enSys.sys.network).sort()
    );
  });

  it('has every mapped key present in both zh and en locales', () => {
    for (const key of [...Object.values(EXACT_MESSAGES), ...PREFIX_MESSAGES.map(([, k]) => k)]) {
      expect(tZh(key, `__missing_zh__${key}`), `zh missing ${key}`).not.toContain('__missing_zh__');
      expect(tEn(key, `__missing_en__${key}`), `en missing ${key}`).not.toContain('__missing_en__');
    }
  });
});

describe('regStateLabel', () => {
  it('localizes the known registration states', () => {
    expect(regStateLabel('registered', t)).toBe(
      '[[sys.cellular.reg_state_registered]]registered'
    );
    expect(regStateLabel('not_registered', t)).toBe(
      '[[sys.cellular.reg_state_not_registered]]not registered'
    );
  });

  it('passes unknown states through', () => {
    expect(regStateLabel('weird_state', t)).toBe('weird_state');
    expect(regStateLabel('', t)).toBe('');
  });
});
