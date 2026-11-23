/**
 * PTS-based presentation reorder buffer.
 *
 * Some devices send frames in decode order while assigning PTS in presentation order.
 * Preserve decode order for reference frames, then reorder decoder output by PTS.
 *
 * Release frames once their PTS trails the maximum seen by the release window.
 * The window must cover the stream's maximum reordering depth, adding latency.
 * PacedPresenter can smooth burst releases. flushBefore can drain an old GOP.
 * Signed differences handle unsigned 32-bit timestamp wraparound.
 */

export interface PtsReorderOptions<T = unknown> {
  /** Release window in 90 kHz ticks; defaults to 333 ms. */
  releaseWindowTicks?: number;
  /** Maximum buffer depth; excess oldest frames are released immediately. Defaults to 16. */
  maxDepth?: number;
  /**
   * Backward PTS jump threshold in 90 kHz ticks. A larger jump resets the buffer
   * for a stream restart or loop. Defaults to 667 ms.
   */
  resetThresholdTicks?: number;
  /** Recycle resources discarded when the stream restarts. */
  onDiscard?: (items: T[]) => void;
}

export interface ReleasedEntry<T> {
  pts: number;
  item: T;
}

interface Entry<T> {
  pts: number;
  item: T;
}

export class PtsReorderBuffer<T> {
  private readonly window: number;
  private readonly maxDepth: number;
  private readonly resetThreshold: number;
  private readonly onDiscard: ((items: T[]) => void) | null;
  private buf: Entry<T>[] = []; // Sorted by PTS.
  private maxSeen: number | null = null;

  constructor(opts: PtsReorderOptions<T> = {}) {
    this.window = opts.releaseWindowTicks ?? 30000;
    this.maxDepth = opts.maxDepth ?? 16;
    this.resetThreshold = opts.resetThresholdTicks ?? 60000;
    this.onDiscard = opts.onDiscard ?? null;
  }

  get size(): number {
    return this.buf.length;
  }

  /** Compare timestamps while accounting for unsigned 32-bit wraparound. */
  private before(a: number, b: number): boolean {
    return ((a - b) | 0) < 0;
  }

  /** Insert a decoded frame and return releasable frames in PTS order. */
  push(pts: number, item: T): ReleasedEntry<T>[] {
    // Reset the release window after a large backward timeline jump.
    if (this.maxSeen !== null && ((this.maxSeen - pts) | 0) > this.resetThreshold) {
      const dropped = this.buf;
      this.buf = [];
      this.maxSeen = null;
      this.onDiscard?.(dropped.map(e => e.item));
    }
    if (this.maxSeen === null || this.before(this.maxSeen, pts)) this.maxSeen = pts;
    let i = this.buf.length;
    while (i > 0 && this.before(pts, this.buf[i - 1].pts)) i--;
    this.buf.splice(i, 0, { pts, item });
    return this.release();
  }

  /** Release frames strictly earlier than the given PTS. */
  flushBefore(pts: number): ReleasedEntry<T>[] {
    const out: ReleasedEntry<T>[] = [];
    while (this.buf.length > 0 && this.before(this.buf[0].pts, pts)) out.push(this.buf.shift()!);
    return out;
  }

  /** Release all buffered frames. */
  flush(): ReleasedEntry<T>[] {
    const out = this.buf.slice();
    this.buf = [];
    return out;
  }

  private release(): ReleasedEntry<T>[] {
    const out: ReleasedEntry<T>[] = [];
    while (this.buf.length > 0) {
      // Earlier frames may still arrive while the queue head is within the window.
      const behind = (this.maxSeen! - this.buf[0].pts) | 0;
      if (behind < this.window) break;
      out.push(this.buf.shift()!);
    }
    while (this.buf.length > this.maxDepth) out.push(this.buf.shift()!);
    return out;
  }
}
