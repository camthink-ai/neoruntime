// @ts-nocheck
/** Schedule decoded frames against an audio clock on a shared 90 kHz timeline. */
export class AudioPresenter<T> {
  private queue: { item: T; pts: number; queuedAt: number }[] = [];
  private timer: ReturnType<typeof setTimeout> | null = null;
  private lastDrift: number | null = null;
  private dropped = 0;
  private status: 'waiting' | 'active' | 'unavailable' = 'waiting';

  constructor(private readonly opts: {
    clock: () => number | null;
    present: (item: T) => void;
    discard: (item: T) => void;
    maxDriftMs: number;
    now?: () => number;
  }) {}

  push(item: T, pts: number): void {
    this.queue.push({ item, pts, queuedAt: this.now() });
    while (this.queue.length > 24) this.dropFirst();
    this.tick();
  }

  snapshot() { return { status: this.status, driftMs: this.lastDrift, dropped: this.dropped, queued: this.queue.length }; }

  untimed(): void { this.status = 'unavailable'; this.lastDrift = null; }

  clear(): void {
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    for (const entry of this.queue) this.opts.discard(entry.item);
    this.queue = [];
    this.lastDrift = null;
    this.status = 'waiting';
  }

  private now(): number { return this.opts.now?.() ?? performance.now(); }
  private dropFirst(): void { const entry = this.queue.shift(); if (entry) { this.dropped++; this.opts.discard(entry.item); } }

  private tick(): void {
    if (this.timer !== null) clearTimeout(this.timer);
    this.timer = null;
    while (this.queue.length) {
      const frame = this.queue[0];
      const audioPts = this.opts.clock();
      if (audioPts === null) {
        this.lastDrift = null;
        // Avoid stalling indefinitely when the source does not deliver usable timestamps.
        if (this.status === 'unavailable' || this.now() - frame.queuedAt >= 1000) { this.status = 'unavailable'; this.opts.present(this.queue.shift()!.item); continue; }
        this.status = 'waiting';
        break;
      }
      const drift = ((Math.round(frame.pts) - Math.round(audioPts)) | 0) / 90;
      this.lastDrift = drift;
      this.status = 'active';
      // Different timestamp epochs are not a valid synchronization signal.
      if (Math.abs(drift) > 10000) { this.status = 'unavailable'; this.lastDrift = null; this.opts.present(this.queue.shift()!.item); continue; }
      if (drift > 5) break;
      if (drift < -this.opts.maxDriftMs && this.queue.length > 1) this.dropFirst();
      else this.opts.present(this.queue.shift()!.item);
    }
    if (this.queue.length) this.timer = setTimeout(() => this.tick(), 10);
  }
}
