import { describe, expect, it } from 'vitest';
import { resolvePlayerObjectFit } from './objectFit';

describe('resolvePlayerObjectFit', () => {
  it('fills 16:9-or-wider video without side bars', () => {
    expect(resolvePlayerObjectFit('adaptive', 1920, 1080)).toBe('cover');
    expect(resolvePlayerObjectFit('adaptive', 2560, 1080)).toBe('cover');
  });

  it('keeps narrower-than-16:9 video fully visible', () => {
    // 4:3 landscape must not be cropped to fill the 16:9 preview.
    expect(resolvePlayerObjectFit('adaptive', 640, 480)).toBe('contain');
    expect(resolvePlayerObjectFit('adaptive', 1920, 1920)).toBe('contain');
  });

  it('keeps portrait video fully visible', () => {
    expect(resolvePlayerObjectFit('adaptive', 1080, 1920)).toBe('contain');
  });

  it('does not crop before dimensions are known', () => {
    expect(resolvePlayerObjectFit('adaptive', 0, 0)).toBe('contain');
  });

  it('passes explicit modes through unchanged', () => {
    expect(resolvePlayerObjectFit('cover', 640, 480)).toBe('cover');
    expect(resolvePlayerObjectFit('contain', 1920, 1080)).toBe('contain');
  });
});
