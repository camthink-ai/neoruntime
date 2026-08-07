import { describe, expect, it } from 'vitest';

import { shouldAutoSyncFromClient } from './autoSync';

describe('shouldAutoSyncFromClient', () => {
  it('does not sync when persisted mode is manual', () => {
    expect(
      shouldAutoSyncFromClient({
        deviceUnixTimestamp: 1000,
        browserUnixTimestamp: 2000,
        persistedSyncMode: 'manual',
        formSyncMode: 'manual',
        autoSync: false,
      })
    ).toBe(false);
  });

  it('does not sync while the form is set to manual before config refreshes', () => {
    expect(
      shouldAutoSyncFromClient({
        deviceUnixTimestamp: 1000,
        browserUnixTimestamp: 2000,
        persistedSyncMode: 'ntp',
        formSyncMode: 'manual',
        autoSync: true,
      })
    ).toBe(false);
  });

  it('syncs only when auto-sync is enabled and the diff is significant', () => {
    expect(
      shouldAutoSyncFromClient({
        deviceUnixTimestamp: 1000,
        browserUnixTimestamp: 1061,
        persistedSyncMode: 'ntp',
        formSyncMode: 'ntp',
        autoSync: true,
      })
    ).toBe(true);

    expect(
      shouldAutoSyncFromClient({
        deviceUnixTimestamp: 1000,
        browserUnixTimestamp: 1060,
        persistedSyncMode: 'ntp',
        formSyncMode: 'ntp',
        autoSync: true,
      })
    ).toBe(false);
  });
});
