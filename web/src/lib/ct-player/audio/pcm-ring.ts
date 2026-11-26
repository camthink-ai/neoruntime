// @ts-nocheck
/**
 * PCM ring buffer and format conversion. Prebuffer at startup and after an
 * underrun; discard the oldest samples when the buffer exceeds its limit.
 */

export class PcmRing {
  private chunks: Float32Array[] = [];
  private frames = 0;
  private readonly sampleRate: number;

  constructor(sampleRate = 48000, private readonly maxBufferMs = 1200) {
    this.sampleRate = sampleRate;
  }

  /** Convert interleaved S16LE to Float32; currently only the first channel is consumed. */
  static toFloat(int16: Int16Array): Float32Array {
    const out = new Float32Array(int16.length);
    for (let i = 0; i < int16.length; i++) out[i] = int16[i] / 32768;
    return out;
  }

  push(chunk: Float32Array): void {
    this.chunks.push(chunk);
    this.frames += chunk.length;
    const max = (this.maxBufferMs / 1000) * this.sampleRate;
    while (this.frames > max && this.chunks.length > 1) {
      const c = this.chunks.shift()!;
      this.frames -= c.length;
    }
  }

  /** Read up to n frames, returning only the available samples. */
  pull(n: number): Float32Array | null {
    if (this.chunks.length === 0) return null;
    const out = new Float32Array(Math.min(n, this.frames));
    let off = 0;
    while (off < out.length && this.chunks.length > 0) {
      const c = this.chunks[0];
      const take = Math.min(c.length, out.length - off);
      out.set(c.subarray(0, take), off);
      if (take === c.length) this.chunks.shift();
      else this.chunks[0] = c.subarray(take) as Float32Array;
      off += take;
      this.frames -= take;
    }
    return out.length > 0 ? out : null;
  }

  get bufferedFrames(): number {
    return this.frames;
  }

  get bufferedMs(): number {
    return (this.frames / this.sampleRate) * 1000;
  }

  clear(): void {
    this.chunks = [];
    this.frames = 0;
  }
}
