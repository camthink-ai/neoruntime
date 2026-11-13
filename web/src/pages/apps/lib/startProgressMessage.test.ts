import { describe, expect, it, vi } from 'vitest';
import { translateStartProgress } from './startProgressMessage';

const t = vi.fn((key: string, fallback: string) => `${key}:${fallback}`) as any;

describe('translateStartProgress', () => {
  it('maps backend start phases to localized keys', () => {
    expect(translateStartProgress({ phase: 'restoring_image' }, t)).toContain(
      'sys.apps.start_progress.restoring_image'
    );
    expect(translateStartProgress({ phase: 'preloading_models' }, t)).toContain(
      'sys.apps.start_progress.preloading_models'
    );
    expect(translateStartProgress({ phase: 'starting_container' }, t)).toContain(
      'sys.apps.start_progress.starting_container'
    );
  });

  it('falls back to image checking before progress is available', () => {
    expect(translateStartProgress(undefined, t)).toContain(
      'sys.apps.start_progress.checking_image'
    );
  });
});
