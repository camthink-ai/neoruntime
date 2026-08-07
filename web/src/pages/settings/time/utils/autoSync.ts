export const CLIENT_TIME_AUTOSYNC_DIFF_SECONDS = 60;

interface ShouldAutoSyncFromClientParams {
  deviceUnixTimestamp?: number;
  browserUnixTimestamp: number;
  persistedSyncMode?: string;
  formSyncMode?: string;
  autoSync?: boolean;
}

export function shouldAutoSyncFromClient({
  deviceUnixTimestamp,
  browserUnixTimestamp,
  persistedSyncMode,
  formSyncMode,
  autoSync,
}: ShouldAutoSyncFromClientParams): boolean {
  if (!deviceUnixTimestamp) return false;
  if (persistedSyncMode === 'manual' || formSyncMode === 'manual') {
    return false;
  }
  if (autoSync === false) return false;

  const diff = Math.abs(browserUnixTimestamp - deviceUnixTimestamp);
  return diff > CLIENT_TIME_AUTOSYNC_DIFF_SECONDS;
}
