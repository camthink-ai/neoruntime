import { describe, expect, it, vi } from 'vitest';
import { refreshMediaStatusUntilStreamActive } from './mediaStatusRecovery';

describe('refreshMediaStatusUntilStreamActive', () => {
  it('retries a transient inactive status until the stream is active', async () => {
    vi.useFakeTimers();
    const statuses = [
      { status: 'inactive', has_encoder: false },
      { status: 'active', has_encoder: true },
    ];
    let current = statuses[0];
    const queryClient = {
      refetchQueries: vi.fn(async () => {
        current = statuses.shift() ?? current;
      }),
      getQueryData: vi.fn(() => ({
        streams: [{ stream_id: 'main', ...current }],
      })),
    };

    const resultPromise = refreshMediaStatusUntilStreamActive(
      queryClient as any,
      'main',
      { retryCount: 3, retryDelayMs: 100 }
    );

    await vi.advanceTimersByTimeAsync(100);

    await expect(resultPromise).resolves.toBe(true);
    expect(queryClient.refetchQueries).toHaveBeenCalledTimes(2);
  });
});
