// @ts-nocheck
/**
 * Adapter utilities: NaluFramer converts NAL units into normalized EncodedFrame objects.
 *
 * Access-unit heuristic: a new VCL NAL closes an access unit that already contains VCL.
 * Parameter sets are included in the following access unit. Unclassifiable NAL units
 * are dropped until the codec is known.
 */

import type { EncodedFrame, VideoCodec } from '../shared/types';
import {
  bytesEqual,
  isH264Vcl,
  isHevcRap,
  isHevcVcl,
  nalCodecHint,
  scanAnnexBNalus,
} from '../media/nalu';

export interface FramerTimestamp {
  pts90k?: number;
  dts90k?: number;
}

export interface FramerOptions {
  /**
   * 'stream': NAL units may span chunks (default).
   * 'message': each chunk contains complete access units.
   */
  framing?: 'stream' | 'message';
  /** Maximum buffer size; exceeding it resets the buffer. */
  maxBufferBytes?: number;
}

interface NaluRecord {
  /** Payload without a start code. */
  payload: Uint8Array;
  /** NAL type in the current codec. */
  type: number;
  vcl: boolean;
}

const START_CODE = new Uint8Array([0, 0, 0, 1]);

export function concatBytes(a: Uint8Array, b: Uint8Array): Uint8Array {
  if (a.length === 0) return b;
  if (b.length === 0) return a;
  const out = new Uint8Array(a.length + b.length);
  out.set(a, 0);
  out.set(b, a.length);
  return out;
}

/** Detect an Annex B start code within the first maxScan bytes. */
export function looksLikeAnnexB(data: Uint8Array, maxScan = 4096): boolean {
  const n = Math.min(data.length, maxScan);
  for (let i = 0; i + 3 < n; i++) {
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) return true;
  }
  return false;
}

/**
 * Convert a sequence of four-byte big-endian length-prefixed AVCC NAL units to Annex B; return null for invalid input.
 */
export function avccToAnnexB(data: Uint8Array): Uint8Array | null {
  const parts: Uint8Array[] = [];
  let idx = 0;
  while (idx + 4 <= data.length) {
    const len =
      ((data[idx] << 24) | (data[idx + 1] << 16) | (data[idx + 2] << 8) | data[idx + 3]) >>> 0;
    idx += 4;
    if (len <= 0 || idx + len > data.length) return null;
    parts.push(START_CODE, data.subarray(idx, idx + len));
    idx += len;
  }
  if (idx !== data.length || parts.length === 0) return null;
  const total = parts.reduce((s, p) => s + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}

/** Convert one AVCC NAL unit to Annex B. */
export function singleAvccToAnnexB(data: Uint8Array): Uint8Array | null {
  if (data.length < 5) return null;
  const len = ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) >>> 0;
  if (len <= 0 || len + 4 !== data.length) return null;
  return concatBytes(START_CODE, data.subarray(4));
}

export class NaluFramer {
  private buf: Uint8Array = new Uint8Array(0);
  private pendingAu: NaluRecord[] = [];
  private readonly framing: 'stream' | 'message';
  private readonly maxBufferBytes: number;
  private seq = 0;

  /** Most recent SPS payload for change detection. */
  lastSps: Uint8Array | null = null;

  constructor(opts: FramerOptions = {}) {
    this.framing = opts.framing ?? 'stream';
    this.maxBufferBytes = opts.maxBufferBytes ?? 2 * 1024 * 1024;
  }

  private codecInternal: VideoCodec | null = null;

  get codec(): VideoCodec | null {
    return this.codecInternal;
  }

  reset(): void {
    this.buf = new Uint8Array(0);
    this.codecInternal = null;
    this.lastSps = null;
    this.pendingAu = [];
  }

  /**
   * Feed a byte chunk. In message mode call endMessage() after each push.
   * Returns complete frames produced by this call.
   */
  push(chunk: Uint8Array, ts?: FramerTimestamp): EncodedFrame[] {
    this.buf = concatBytes(this.buf, chunk);
    if (this.buf.length > this.maxBufferBytes) {
      this.buf = new Uint8Array(0);
      this.pendingAu = [];
      return [];
    }
    const scanned = scanAnnexBNalus(this.buf);
    // Preserve a possible start code split across transport messages.
    let keepFrom = scanned.length === 0 ? Math.max(0, this.buf.length - 3) : this.buf.length;
    for (const n of scanned) {
      if (n.partial) {
        keepFrom = n.start;
        break;
      }
    }
    const frames: EncodedFrame[] = [];
    for (const n of scanned) {
      if (n.partial) continue;
      this.feedNalu(n, frames, ts);
      // 关闭的 AU 记录持有旧缓冲的视图（旧缓冲仍被引用，GC 安全），
      // 无需回退 keepFrom——回退会让 endMessage 重复喂数据
    }
    this.buf = keepFrom < this.buf.length ? this.buf.slice(keepFrom) : new Uint8Array(0);
    return frames;
  }

  /** End a message, treating its trailing NAL as complete and flushing the access unit. */
  endMessage(ts?: FramerTimestamp): EncodedFrame[] {
    const frames: EncodedFrame[] = [];
    const scanned = scanAnnexBNalus(this.buf);
    for (const n of scanned) {
      this.feedNalu(n, frames, ts);
    }
    this.buf = new Uint8Array(0);
    const last = this.emitPending(ts);
    if (last) frames.push(last);
    return frames;
  }

  /** Flush pending data at end of stream. */
  flush(ts?: FramerTimestamp): EncodedFrame[] {
    return this.endMessage(ts);
  }

  private feedNalu(
    n: { header: number; rawHeader: number; payloadLen: number },
    frames: EncodedFrame[],
    ts?: FramerTimestamp
  ): boolean {
    if (n.payloadLen <= 0) return false;
    if (this.codecInternal === null) {
      const hint = nalCodecHint(n.rawHeader);
      if (!hint) return false; // Discard unclassifiable NAL units until parameter sets identify the codec.
      this.codecInternal = hint;
    }
    const codec = this.codecInternal;
    const type = codec === 'h264' ? n.rawHeader & 0x1f : (n.rawHeader >> 1) & 0x3f;
    const vcl = codec === 'h264' ? isH264Vcl(type) : isHevcVcl(type);

    let closedAu = false;
    if (vcl && this.pendingAu.some((r) => r.vcl)) {
      const frame = this.emitPending(ts);
      if (frame) frames.push(frame);
      closedAu = true;
    }
    this.pendingAu.push({
      payload: this.buf.subarray(n.header, n.header + n.payloadLen),
      type,
      vcl,
    });
    return closedAu;
  }

  private emitPending(ts?: FramerTimestamp): EncodedFrame | null {
    if (this.pendingAu.length === 0 || this.codec === null) return null;
    const records = this.pendingAu;
    this.pendingAu = [];

    let total = 0;
    for (const r of records) total += 4 + r.payload.length;
    const data = new Uint8Array(total);
    const nalus: EncodedFrame['nalus'] = [];
    let off = 0;
    let keyframe = false;
    for (const r of records) {
      data.set(START_CODE, off);
      data.set(r.payload, off + 4);
      nalus.push({ offset: off, size: 4 + r.payload.length, type: r.type });
      if (r.vcl) {
        if (this.codec === 'h264' ? r.type === 5 : isHevcRap(r.type)) keyframe = true;
      }
      off += 4 + r.payload.length;
    }
    // Track SPS changes for the pipeline.
    const spsType = this.codec === 'h264' ? 7 : 33;
    for (let i = 0; i < records.length; i++) {
      if (records[i].type === spsType) {
        const payload = data.subarray(nalus[i].offset + 4, nalus[i].offset + nalus[i].size);
        if (!this.lastSps || !bytesEqual(this.lastSps, payload)) this.lastSps = payload.slice();
      }
    }
    return {
      codec: this.codec,
      data,
      nalus,
      keyframe,
      pts90k: ts?.pts90k,
      dts90k: ts?.dts90k,
      seq: this.seq++,
    };
  }
}

/** Normalize Annex B or length-prefixed AVCC payloads to Annex B. */
export function normalizeToAnnexB(payload: Uint8Array): Uint8Array {
  // A length prefix can itself contain 00 00 01. Validate the complete
  // length-prefixed sequence before looking for Annex B start codes.
  const multi = avccToAnnexB(payload);
  if (multi) return multi;
  if (looksLikeAnnexB(payload)) return payload;
  return payload; // Pass unknown input to the framer, which limits buffer growth.
}
