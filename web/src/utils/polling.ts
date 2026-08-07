import { sleep } from '@/utils';

export interface PollingOptions<T> {
  /** API function to call each interval */
  fn: () => Promise<T>;
  /** Callback when fn resolves successfully — return true to stop polling */
  onSuccess: (result: T) => boolean;
  /** Interval between calls in ms, default 3000 */
  interval?: number;
  /** Max total duration in ms before giving up, default 60000 */
  timeout?: number;
  /** Called when timeout is exceeded */
  onTimeout?: () => void;
  /** Called when fn throws an error (polling continues unless you throw) */
  onError?: (error: unknown) => void;
}

export interface PollingHandle {
  stop: () => void;
}

/**
 * Poll an async function at a fixed interval until success or timeout.
 * Returns a handle with a stop() method to cancel early.
 */
export function startPolling<T>(options: PollingOptions<T>): PollingHandle {
  const {
    fn,
    onSuccess,
    interval = 3000,
    timeout = 60000,
    onTimeout,
    onError,
  } = options;

  let stopped = false;
  const deadline = Date.now() + timeout;

  const run = async () => {
    const tick = (): Promise<void> => new Promise(resolve => {
        if (stopped) {
          resolve();
          return;
        }
        if (Date.now() >= deadline) {
          if (!stopped) onTimeout?.();
          resolve();
          return;
        }
        fn()
          .then(result => {
            if (stopped) {
              resolve();
              return;
            }
            const done = onSuccess(result);
            if (done) {
              resolve();
              return;
            }
            sleep(interval).then(() => tick().then(resolve));
          })
          .catch(err => {
            if (stopped) {
              resolve();
              return;
            }
            onError?.(err);
            sleep(interval).then(() => tick().then(resolve));
          });
      });
    await tick();
  };

  run();

  return {
    stop: () => {
      stopped = true;
    },
  };
}
