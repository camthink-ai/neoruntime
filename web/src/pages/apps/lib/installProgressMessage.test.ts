import { describe, expect, it } from 'vitest';
import {
  translateInstallPhase,
  translateInstallProgress,
} from '@/pages/apps/lib/installProgressMessage';

const t = ((key: string, fallback?: string) => fallback ?? key) as never;

describe('installProgressMessage', () => {
  it('translates known backend progress messages via fallback', () => {
    expect(
      translateInstallProgress(
        { phase: 'pulling', message: 'Importing local image...' },
        t
      )
    ).toBe('Importing local image...');
  });

  it('uses phase fallback when message is unknown', () => {
    expect(
      translateInstallProgress({ phase: 'registering', message: '' }, t)
    ).toBe('Registering application...');
  });

  it('translates install phase label', () => {
    expect(translateInstallPhase('pulling', t)).toBe('Image');
  });
});
