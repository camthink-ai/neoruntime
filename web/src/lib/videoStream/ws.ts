let ws: WebSocket | null = null;
const ctx: any = globalThis as any;

let currentUrl: string | null = null;
let shouldReconnect = false;

let retryCount = 0;
const maxRetryCount = 10;
const retryWindowMs = 5 * 60 * 1000;
let firstRetryTime: number | null = null;
const baseDelay = 1000; // ms
let retryTimer: number | null = null;

// Fallback deadline for a WebSocket stuck in CONNECTING. safeClose on a
// CONNECTING socket only closes once `open` fires; if the handshake never
// completes (server not accepting, TCP half-open) the socket — and its
// backend connection — would leak forever. After this many ms, force close.
const wsConnectFallbackMs = 4000;

function clearRetryTimer(): void {
  if (retryTimer !== null) {
    clearTimeout(retryTimer);
    retryTimer = null;
  }
}

function safeClose(socket: WebSocket): void {
  try {
    if (socket.readyState === WebSocket.CONNECTING) {
      // If the socket is still handshaking, close on open — but also arm a
      // fallback timer: a socket that never reaches OPEN (e.g. TCP stuck,
      // server not accepting) would otherwise leak and hold a backend
      // connection forever. Force-close after wsConnectFallbackMs regardless.
      const forceTimer = setTimeout(() => {
        try {
          if (
            socket.readyState === WebSocket.OPEN
            || socket.readyState === WebSocket.CONNECTING
          ) {
            socket.close(1000, 'teardown-timeout');
          }
        } catch (error) {
          ctx.postMessage({ type: 'error', error: String(error) });
        }
      }, wsConnectFallbackMs);
      socket.addEventListener(
        'open',
        () => {
          clearTimeout(forceTimer);
          try {
            socket.close(1000, 'teardown');
          } catch (error) {
            ctx.postMessage({ type: 'error', error: String(error) });
          }
        },
        { once: true }
      );
      socket.addEventListener('close', () => clearTimeout(forceTimer), {
        once: true,
      });
    } else if (
      socket.readyState === WebSocket.OPEN
      || socket.readyState === WebSocket.CLOSING
    ) {
      socket.close(1000, 'teardown');
    }
  } catch (error) {
    ctx.postMessage({ type: 'error', error: String(error) });
  }
}

function teardown(resetUrl = false): void {
  if (ws) {
    const socket = ws;
    ws = null;
    safeClose(socket);
  }

  if (resetUrl) {
    currentUrl = null;
  }

  clearRetryTimer();
}

function scheduleReconnect(): void {
  if (!shouldReconnect || !currentUrl) {
    return;
  }

  const now = Date.now();

  // If this is the first reconnection, record the timestamp
  if (firstRetryTime === null) {
    firstRetryTime = now;
  }

  // Check if more than 1 minute have passed
  if (now - firstRetryTime >= retryWindowMs) {
    // If more than 10 minutes have passed, reset the count and timestamp
    retryCount = 0;
    firstRetryTime = now;
  }

  // Check the number of reconnection attempts in the last 1 minute
  if (retryCount >= maxRetryCount) {
    ctx.postMessage({
      type: 'close',
      code: 4999,
      reason: 'Max retries reached in 10 minutes',
    });
    return;
  }

  retryCount += 1;
  const expo = Math.min(baseDelay * 2 ** (retryCount - 1), 10000);
  const jitter = Math.floor(Math.random() * 300);
  const delay = expo + jitter;
  clearRetryTimer();
  retryTimer = setTimeout(() => {
    attemptConnect(currentUrl!);
  }, delay) as unknown as number;
}

function attemptConnect(url: string): void {
  teardown(false);

  try {
    // Notify main thread: connection is starting (used to show loading UI)
    ctx.postMessage({ type: 'connecting' });
    const socket = new WebSocket(url);
    ws = socket;
    socket.binaryType = 'arraybuffer';

    socket.onopen = () => {
      if (ws !== socket) {
        safeClose(socket);
        return;
      }
      retryCount = 0;
      firstRetryTime = null; // Reset the time window
      clearRetryTimer();
      ctx.postMessage({ type: 'open' });
    };

    socket.onclose = (event: CloseEvent) => {
      const isCurrentSocket = ws === socket;
      const wasManual = !shouldReconnect;
      if (isCurrentSocket) {
        ws = null;
        if (!shouldReconnect) {
          currentUrl = null;
        }
        clearRetryTimer();
      }

      // Debug: log close event
      console.log(
        '[WebSocket] Closed: code=',
        event.code,
        'reason=',
        event.reason,
        'wasManual=',
        wasManual,
        'shouldReconnect=',
        shouldReconnect,
        'isCurrentSocket=',
        isCurrentSocket
      );

      if (!isCurrentSocket) {
        return;
      }

      if (wasManual) {
        ctx.postMessage({
          type: 'close',
          code: event.code,
          reason: event.reason,
        });
        return;
      }

      // Backend intentionally replaced this older preview connection with a
      // newer one from the same client IP. Do not reconnect the old tab and
      // fight the fresh preview for the per-IP slot.
      if (event.reason === 'Connection replaced' || event.code === 1008) {
        shouldReconnect = false;
        ctx.postMessage({
          type: 'close',
          code: event.code,
          reason: event.reason,
        });
        return;
      }

      // 1000-1006: Normal/Strategic close does not reconnect
      if (event.code >= 1000 && event.code <= 1005) {
        ctx.postMessage({
          type: 'close',
          code: event.code,
          reason: event.reason,
        });
        return;
      }
      scheduleReconnect();
    };

    socket.onerror = () => {
      if (ws !== socket) {
        return;
      }
      ctx.postMessage({ type: 'error', error: 'WebSocket error' });
      // Browsers fire `close` after `error`; reconnect from `close` only.
      // Scheduling here as well creates overlapping retry loops.
    };

    socket.onmessage = (event: MessageEvent) => {
      if (ws !== socket) {
        return;
      }
      const payload = event.data;
      const message = { type: 'video-data', payload } as const;
      if (payload instanceof ArrayBuffer) {
        ctx.postMessage(message, [payload]);
      } else {
        ctx.postMessage(message);
      }
    };
  } catch (error) {
    ctx.postMessage({ type: 'error', error: String(error) });
    scheduleReconnect();
  }
}

(globalThis as any).onmessage = (e: MessageEvent) => {
  const data = e.data || {};
  switch (data.type) {
    case 'connect': {
      const { url } = data;
      if (!url) {
        ctx.postMessage({ type: 'error', error: 'Missing url for connect' });
        return;
      }

      if (ws && currentUrl === url) {
        const state = ws.readyState;
        if (state === WebSocket.OPEN || state === WebSocket.CONNECTING) {
          // Already connected or connected to the same URL
          return;
        }
      }

      shouldReconnect = true;
      currentUrl = url;
      retryCount = 0;
      firstRetryTime = null; // Reset the time window
      clearRetryTimer();
      attemptConnect(url);
      break;
    }
    case 'disconnect': {
      shouldReconnect = false;
      teardown(true);
      break;
    }
    case 'send': {
      const { payload } = data;
      if (ws && ws.readyState === WebSocket.OPEN) {
        try {
          ws.send(payload as any);
        } catch (error) {
          ctx.postMessage({ type: 'error', error: String(error) });
        }
      }
      break;
    }
    default:
      break;
  }
};
