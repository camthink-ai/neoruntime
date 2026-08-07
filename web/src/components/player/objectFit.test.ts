import { describe, expect, it } from 'vitest';
import { resolvePlayerObjectFit } from './objectFit';

describe('resolvePlayerObjectFit', () => {
  it('fills landscape video without side bars', () => {
    expect(resolvePlayerObjectFit('adaptive', 1920, 1080)).toBe('cover');
    expect(resolvePlayerObjectFit('adaptive', 1920, 1920)).toBe('cover');
  });

  it('keeps portrait video fully visible', () => {
    expect(resolvePlayerObjectFit('adaptive', 1080, 1920)).toBe('contain');
  });

  it('does not crop before dimensions are known', () => {
    expect(resolvePlayerObjectFit('adaptive', 0, 0)).toBe('contain');
  });
});
