// @ts-nocheck
/**
 * Present frames released in batches according to their PTS intervals.
 *
 * PtsReorderBuffer can release frames in bursts. A timer presents them at their
 * scheduled times; if several frames are overdue, only the newest is presented.
 */

import type { ReleasedEntry } from './reorder';

export interface PacedPresenterOptions<T> {
  /** Initial presentation buffer in milliseconds. */
  jitterMs: number;
  /** Maximum queue size; excess frames with the earliest due time are discarded. */
  maxPending: number;
  /** Optional maximum PTS scheduling drift in milliseconds before resetting the timeline. */
  maxLeadMs?: number;
  /** Re-anchor to the newest frame when a live presentation queue overflows. */
  resyncOnOverflow?: boolean;
  present: (item: T) => void;
  /** Recycle discarded items when overdue frames are collapsed or the queue is cleared. */
  discard: (item: T) => void;
  now?: () => number;
}

interface Queued<T> {
  pts: number;
  item: T;
  dueMs: number;
}

export class PacedPresenter<T> {
  private queue: Queued<T>[] = []; // Sorted by due time.
  private epochMs: number | null = null;
  private basePts: number | null = null;
  private timer: ReturnType<typeof setTimeout> | null = null;
  private resets = 0;

  constructor(private readonly opts: PacedPresenterOptions<T>) {}

  get size(): number {
    return this.queue.length;
  }

  get diagnostics() {
    return { queued: this.queue.length, nextInMs: this.queue.length ? this.queue[0].dueMs - this.now() : null, resets: this.resets };
  }

  private now(): number {
    return this.opts.now?.() ?? performance.now();
  }

  push(entries: ReleasedEntry<T>[]): void {
    if (entries.length === 0) return;
    const now = this.now();
    for (const e of entries) this.insert(e, now);
    if (this.opts.resyncOnOverflow && this.queue.length > this.opts.maxPending) {
      const newest = this.queue.pop()!;
      this.clear();
      this.resets++;
      this.basePts = newest.pts;
      this.epochMs = now + this.opts.jitterMs;
      newest.dueMs = this.epochMs;
      this.queue.push(newest);
    } else {
      while (this.queue.length > this.opts.maxPending) this.opts.discard(this.queue.shift()!.item);
    }
    this.schedule();
  }

  private insert(e: ReleasedEntry<T>, now: number): void {
    if (this.basePts === null || this.epochMs === null) {
      this.basePts = e.pts;
      this.epochMs = now + this.opts.jitterMs;
    }
    let dueMs = this.epochMs + ((e.pts - this.basePts) | 0) / 90;
    if (this.opts.maxLeadMs !== undefined && Math.abs(dueMs - now) > this.opts.maxLeadMs) {
      // Timestamp discontinuities must not leave live frames waiting indefinitely.
      this.clear();
      this.resets++;
      this.basePts = e.pts;
      this.epochMs = now + this.opts.jitterMs;
      dueMs = this.epochMs;
    }
    // Re-anchor after a long interruption to avoid immediately releasing new frames.
    if (this.queue.length === 0 && dueMs < now - 50) {
      this.basePts = e.pts;
      this.epochMs = now + this.opts.jitterMs;
      dueMs = this.epochMs;
    }
    let i = this.queue.length;
    while (i > 0 && this.queue[i - 1].dueMs > dueMs) i--;
    this.queue.splice(i, 0, { pts: e.pts, item: e.item, dueMs });
  }

  private schedule(): void {
    if (this.timer !== null) return;
    const next = this.queue[0]?.dueMs;
    if (next === undefined) return;
    const delay = Math.max(0, Math.min(16, next - this.now()));
    this.timer = setTimeout(() => {
      this.timer = null;
      this.tick();
    }, delay);
  }

  private tick(): void {
    const now = this.now();
    while (this.queue.length > 0 && this.queue[0].dueMs <= now) {
      const item = this.queue.shift()!;
      // When several frames are overdue, discard all but the newest.
      if (this.queue.length > 0 && this.queue[0].dueMs <= now) this.opts.discard(item.item);
      else this.opts.present(item.item);
    }
    this.schedule();
  }

  /** Discard queued items and stop the timer. */
  clear(): void {
    if (this.timer !== null) {
      clearTimeout(this.timer);
      this.timer = null;
    }
    for (const q of this.queue) this.opts.discard(q.item);
    this.queue = [];
    this.basePts = null;
    this.epochMs = null;
  }
}
