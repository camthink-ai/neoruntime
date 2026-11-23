/** Drawing call intervals, not GPU completion or display presentation timing. */
export class DrawTiming {
  private last: number | null = null;
  private samples: { at: number; gap: number }[] = [];
  private stalls = 0;

  record(now: number): void {
    if (this.last !== null) {
      const gap = Math.max(0, now - this.last);
      this.samples.push({ at: now, gap });
      if (gap > 200) this.stalls++;
      if (this.samples.length > 2000) this.samples = this.samples.slice(-2000);
    }
    this.last = now;
  }

  pause(): void { this.last = null; this.samples = []; }

  snapshot(now: number) {
    this.prune(now);
    const gaps = this.samples.map(sample => sample.gap).sort((a, b) => a - b);
    return {
      currentGapMs: this.last === null ? null : Math.max(0, now - this.last),
      p95GapMs: gaps.length ? gaps[Math.ceil(gaps.length * 0.95) - 1] : null,
      stalls: this.stalls,
    };
  }

  private prune(now: number): void {
    this.samples = this.samples.filter(sample => now - sample.at <= 5000).slice(-2000);
  }
}
