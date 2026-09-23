import { describe, expect, it } from 'vitest';

import {
  MEMORY_PRESETS,
  RESTART_POLICY_PRESETS,
  memoryOptionsFor,
  memoryToBytes,
  restartPoliciesFor,
} from './formFieldOptions';

describe('memoryToBytes', () => {
  it('converts Ki/Mi/Gi/Ti suffixes to byte counts', () => {
    expect(memoryToBytes('64Mi')).toBe(64 * 1024 ** 2);
    expect(memoryToBytes('1.5Gi')).toBe(1.5 * 1024 ** 3);
    expect(memoryToBytes('1Ti')).toBe(1024 ** 4);
  });

  it('treats a bare number as bytes and ignores case and padding', () => {
    expect(memoryToBytes('1024')).toBe(1024);
    expect(memoryToBytes(' 128MI ')).toBe(128 * 1024 ** 2);
  });

  it('returns NaN for unparseable values', () => {
    expect(memoryToBytes('lots')).toBeNaN();
    expect(memoryToBytes('')).toBeNaN();
  });
});

describe('memoryOptionsFor', () => {
  it('returns the presets unchanged when no value is set', () => {
    expect(memoryOptionsFor(undefined)).toEqual([...MEMORY_PRESETS]);
  });

  it('returns the presets unchanged for a preset value', () => {
    expect(memoryOptionsFor('512Mi')).toEqual([...MEMORY_PRESETS]);
  });

  it('inserts an out-of-preset value in ascending byte order', () => {
    expect(memoryOptionsFor('64Mi')).toEqual([
      '64Mi',
      '128Mi',
      '256Mi',
      '512Mi',
      '1Gi',
      '2Gi',
    ]);
    expect(memoryOptionsFor('4Gi')).toEqual([
      '128Mi',
      '256Mi',
      '512Mi',
      '1Gi',
      '2Gi',
      '4Gi',
    ]);
  });

  it('appends an unparseable value at the end rather than dropping it', () => {
    expect(memoryOptionsFor('huge')).toEqual([...MEMORY_PRESETS, 'huge']);
  });
});

describe('restartPoliciesFor', () => {
  it('returns the presets when no value is set', () => {
    expect(restartPoliciesFor(undefined)).toEqual([...RESTART_POLICY_PRESETS]);
  });

  it('returns the presets unchanged for a preset policy', () => {
    expect(restartPoliciesFor('always')).toEqual([...RESTART_POLICY_PRESETS]);
  });

  it('appends an out-of-preset policy so the manifest value stays visible', () => {
    expect(restartPoliciesFor('never')).toEqual([
      'no',
      'on-failure',
      'always',
      'never',
    ]);
  });
});
