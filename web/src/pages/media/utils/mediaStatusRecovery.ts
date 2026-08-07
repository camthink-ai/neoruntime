/* eslint-disable no-await-in-loop */
import type { QueryClient } from '@tanstack/react-query';
import type { MediaStreamStatus } from '@/hooks/useMediaStatus';

const MEDIA_STATUS_QUERY_KEY = ['mediaStatus'] as const;
const DEFAULT_RETRY_COUNT = 10;
const DEFAULT_RETRY_DELAY_MS = 500;

type MediaStatusData = {
  streams: MediaStreamStatus[];
};

type RecoveryOptions = {
  retryCount?: number;
  retryDelayMs?: number;
};

const delay = (ms: number) => new Promise<void>(resolve => {
    window.setTimeout(resolve, ms);
  });

export async function refreshMediaStatusUntilStreamActive(
  queryClient: QueryClient,
  streamName: string,
  options: RecoveryOptions = {}
): Promise<boolean> {
  const retryCount = options.retryCount ?? DEFAULT_RETRY_COUNT;
  const retryDelayMs = options.retryDelayMs ?? DEFAULT_RETRY_DELAY_MS;

  for (let attempt = 0; attempt < retryCount; attempt += 1) {
    await queryClient.refetchQueries({
      queryKey: MEDIA_STATUS_QUERY_KEY,
      type: 'active',
    });

    const status = queryClient.getQueryData<MediaStatusData>(
      MEDIA_STATUS_QUERY_KEY
    );
    const stream = status?.streams.find(item => item.stream_id === streamName);
    if (stream?.status === 'active' && stream.has_encoder) {
      return true;
    }

    if (attempt < retryCount - 1) {
      await delay(retryDelayMs);
    }
  }

  return false;
}
