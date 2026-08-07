import { getItem } from '@/utils/storage';

/**
 * Gyro attitude stream (Server-Sent Events).
 *
 * Consumes `GET /api/v1/monitor/gyro/attitude` — a `text/event-stream` endpoint
 * that pushes the device's two-axis tilt (pitch + roll). Yaw / the Z-axis
 * rotation is intentionally NOT reported: this stream is consumed only by the
 * level-calibration card, which cares about how far the device is off-level,
 * not its heading.
 *
 * Frame convention (Z-up world frame, matches the 3D visual):
 *   - +X = front of the device, +Y = right, +Z = up.
 *   - pitch  front-back tilt in degrees. Positive = front dips down (前倾).
 *   - roll   left-right tilt in degrees. Positive = right dips down (右倾).
 *   - Both are clamped to [-90, 90] by the backend; level is |tilt| within a
 *     small tolerance.
 *
 * Query params (forwarded to the backend):
 *  - token  - auth token (EventSource cannot set headers; same convention as the
 *             audio-talk WebSocket)
 *  - rate   - output frequency cap in Hz, 1..200 (default 50)
 *
 * Events emitted by the backend:
 *  - orientation  - one attitude frame: { pitch: <deg>, roll: <deg> }
 *  - status       - sensor health: online / offline / error
 *  - error        - sensor error detail (sent after a non-online status)
 *  - heartbeat    - keepalive, every 15s
 *
 * Connection lifecycle:
 *  - The backend keeps the stream open across transient sensor unavailability
 *    (it pushes status/error events but does not close), so the client can
 *    recover without reconnecting. It returns 503 (not SSE) only when the gyro
 *    source is disabled server-side — EventSource then transitions to CLOSED
 *    and fires onError (no auto-reconnect); the caller should fall back to a
 *    mock source.
 *  - On transient network errors EventSource auto-reconnects; onError fires
 *    but the connection stays alive.
 */

export interface GyroAttitude {
  /** Front-back tilt in degrees. Positive = front dips down. */
  pitch: number;
  /** Left-right tilt in degrees. Positive = right dips down. */
  roll: number;
}

export type GyroSensorStatus = 'online' | 'offline' | 'error';

export interface GyroSSEHandlers {
  onAttitude: (a: GyroAttitude) => void;
  onOpen?: () => void;
  onError?: () => void;
  /** Fired on each `status` event with the raw sensor health string. */
  onStatus?: (status: string) => void;
}

export interface GyroSSEOptions {
  /** Output frequency cap in Hz, clamped to [1,200] by the backend. Default 50. */
  rate?: number;
}

const ENDPOINT = '/api/v1/monitor/gyro/attitude';

export class GyroSSE {
  private es: EventSource | null = null;

  private readonly handlers: GyroSSEHandlers;

  private readonly url: string;

  constructor(handlers: GyroSSEHandlers, opts: GyroSSEOptions = {}) {
    this.handlers = handlers;

    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) token = token.substring(7);

    const params = new URLSearchParams();
    params.set('token', token);
    if (opts.rate != null) params.set('rate', String(opts.rate));

    this.url = `${ENDPOINT}?${params.toString()}`;
  }

  start() {
    if (this.es) return;
    const es = new EventSource(this.url);
    this.es = es;

    es.onopen = () => this.handlers.onOpen?.();
    es.onerror = () => this.handlers.onError?.();

    // Attitude frames. The backend names the event `orientation` and carries
    // the two-axis tilt as numbers under `pitch` and `roll` (degrees).
    es.addEventListener('orientation', (e: MessageEvent) => {
      try {
        const d = JSON.parse(e.data);
        const pitch = d?.pitch;
        const roll = d?.roll;
        if (
          typeof pitch === 'number'
          && typeof roll === 'number'
          && Number.isFinite(pitch)
          && Number.isFinite(roll)
        ) {
          this.handlers.onAttitude({ pitch, roll });
        }
      } catch {
        /* ignore malformed frame */
      }
    });

    es.addEventListener('status', (e: MessageEvent) => {
      try {
        const d = JSON.parse(e.data);
        if (d && typeof d.sensor === 'string') {
          this.handlers.onStatus?.(d.sensor);
        }
      } catch {
        /* ignore */
      }
    });
  }

  stop() {
    this.es?.close();
    this.es = null;
  }

  get closed() {
    return !this.es || this.es.readyState === EventSource.CLOSED;
  }
}
