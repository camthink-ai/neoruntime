import { describe, expect, it } from 'vitest';

import type { AppTemplate } from '@/services/types';
import { getAppWebUrl } from './appWebUrl';

const app = {
  id: 'model-showcase',
  name: 'Model Showcase',
  state: 'running',
  web_url: '/',
  permissions: {
    network: {
      inbound: [8889],
    },
  },
} satisfies Partial<AppTemplate> as AppTemplate;

describe('getAppWebUrl', () => {
  it('uses the nginx app proxy from the standard HTTPS origin', () => {
    expect(
      getAppWebUrl(app, {
        hostname: '192.168.93.200',
        origin: 'https://192.168.93.200',
        port: '',
      })
    ).toBe('https://192.168.93.200/apps/model-showcase/');
  });

  it('falls back to the direct inbound port from a non-standard console port', () => {
    expect(
      getAppWebUrl(app, {
        hostname: '192.168.93.200',
        origin: 'http://192.168.93.200:8080',
        port: '8080',
      })
    ).toBe('http://192.168.93.200:8889/');
  });

  it('returns null for the direct-port fallback when no inbound port is known', () => {
    expect(
      getAppWebUrl(
        { ...app, permissions: { network: { inbound: [] } } },
        {
          hostname: '192.168.93.200',
          origin: 'http://192.168.93.200:8080',
          port: '8080',
        }
      )
    ).toBeNull();
  });
});
