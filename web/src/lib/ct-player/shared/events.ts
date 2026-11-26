import type {
  CTStats,
  PlayerState,
  StreamChangeRequest,
  TechKind,
} from './types';
import type { CTError } from './errors';

export type Unsubscribe = () => void;

export interface StateEvent {
  state: PlayerState;
  tech: TechKind | null;
  detail?: string;
}

export type FallbackLevel = 'tech' | 'stream' | 'quality';

export interface FallbackEvent {
  from: TechKind | null;
  to: TechKind | 'stream-switch' | 'none';
  level: FallbackLevel;
  reason: string;
}

export interface CTEventMap {
  state: StateEvent;
  stats: CTStats;
  error: CTError;
  fallback: FallbackEvent;
  'request-stream': StreamChangeRequest;
}

export class EventBus {
  private handlers = new Map<string, Set<(payload: unknown) => void>>();

  on<K extends keyof CTEventMap>(
    key: K,
    handler: (payload: CTEventMap[K]) => void
  ): Unsubscribe {
    let set = this.handlers.get(key as string);
    if (!set) {
      set = new Set();
      this.handlers.set(key as string, set);
    }
    set.add(handler as (payload: unknown) => void);
    return () => {
      set?.delete(handler as (payload: unknown) => void);
    };
  }

  emit<K extends keyof CTEventMap>(key: K, payload: CTEventMap[K]): void {
    const set = this.handlers.get(key as string);
    if (!set) return;
    for (const handler of [...set]) {
      try {
        handler(payload);
      } catch (err) {
        // A host callback exception must not interrupt the engine.
        // eslint-disable-next-line no-console
        console.error(`[player] event handler error (${String(key)})`, err);
      }
    }
  }

  /** Whether the event has listeners; an unhandled request-stream is declined immediately. */
  hasHandlers<K extends keyof CTEventMap>(key: K): boolean {
    const set = this.handlers.get(key as string);
    return !!set && set.size > 0;
  }

  clear(): void {
    this.handlers.clear();
  }
}
