/**
 * Minimal HEVC (H.265) fMP4 init segment builder for MSE (hvc1 / hev1).
 * VPS/SPS/PPS NAL units must include the 2-byte HEVC NAL unit header.
 */

function writeU32BE(arr: Uint8Array, offset: number, value: number): void {
  arr[offset] = (value >>> 24) & 0xff;
  arr[offset + 1] = (value >>> 16) & 0xff;
  arr[offset + 2] = (value >>> 8) & 0xff;
  arr[offset + 3] = value & 0xff;
}

function writeU16BE(arr: Uint8Array, offset: number, value: number): void {
  arr[offset] = (value >>> 8) & 0xff;
  arr[offset + 1] = value & 0xff;
}

function writeStr(arr: Uint8Array, offset: number, str: string): void {
  for (let i = 0; i < str.length; i++) {
    arr[offset + i] = str.charCodeAt(i);
  }
}

function box(type: string, ...children: Uint8Array[]): Uint8Array {
  const contentLen = children.reduce((sum, c) => sum + c.length, 0);
  const result = new Uint8Array(8 + contentLen);
  writeU32BE(result, 0, result.length);
  writeStr(result, 4, type);
  let offset = 8;
  for (const child of children) {
    result.set(child, offset);
    offset += child.length;
  }
  return result;
}

function fullBox(
  type: string,
  version: number,
  flags: number,
  content: Uint8Array
): Uint8Array {
  const fullContent = new Uint8Array(4 + content.length);
  fullContent[0] = version;
  fullContent[1] = (flags >>> 16) & 0xff;
  fullContent[2] = (flags >>> 8) & 0xff;
  fullContent[3] = flags & 0xff;
  fullContent.set(content, 4);
  return box(type, fullContent);
}

function concat(...arrays: Uint8Array[]): Uint8Array {
  const totalLen = arrays.reduce((s, a) => s + a.length, 0);
  const result = new Uint8Array(totalLen);
  let offset = 0;
  for (const arr of arrays) {
    result.set(arr, offset);
    offset += arr.length;
  }
  return result;
}

/** Remove HEVC emulation-prevention three-byte sequences from RBSP (NAL after 2-byte header). */
function rbspFromNalUnit(nalWithHeader: Uint8Array): Uint8Array {
  if (nalWithHeader.length <= 2) return new Uint8Array(0);
  const out: number[] = [];
  let i = 2;
  while (i < nalWithHeader.length) {
    if (
      i + 2 < nalWithHeader.length
      && nalWithHeader[i] === 0
      && nalWithHeader[i + 1] === 0
      && nalWithHeader[i + 2] === 3
    ) {
      out.push(0, 0);
      i += 3;
    } else {
      out.push(nalWithHeader[i]);
      i++;
    }
  }
  return new Uint8Array(out);
}

class BitReader {
  private data: Uint8Array;

  private bitPos = 0;

  constructor(data: Uint8Array) {
    this.data = data;
  }

  readBit(): number {
    const byteIdx = this.bitPos >> 3;
    const bitIdx = 7 - (this.bitPos & 7);
    this.bitPos++;
    if (byteIdx >= this.data.length) return 0;
    return (this.data[byteIdx] >> bitIdx) & 1;
  }

  readBits(n: number): number {
    let v = 0;
    for (let k = 0; k < n; k++) v = (v << 1) | this.readBit();
    return v;
  }

  readUE(): number {
    let zeros = 0;
    while (this.readBit() === 0 && zeros < 32) zeros++;
    return zeros > 0 ? (1 << zeros) - 1 + this.readBits(zeros) : 0;
  }
}

export interface HevcPtlInfo {
  generalProfileSpace: number;
  generalTierFlag: number;
  generalProfileIdc: number;
  generalProfileCompatibilityFlags: number;
  generalConstraintIndicatorFlags: Uint8Array;
  generalLevelIdc: number;
}

/**
 * Parse profile_tier_level from SPS RBSP (Annex A main / most device streams).
 */
export function parseHevcSpsPtl(
  spsNalWithHeader: Uint8Array
): HevcPtlInfo | null {
  try {
    const rbsp = rbspFromNalUnit(spsNalWithHeader);
    if (rbsp.length < 12) return null;
    const br = new BitReader(rbsp);
    br.readBits(4); // sps_video_parameter_set_id
    const maxSubLayersMinus1 = br.readBits(3);
    br.readBit(); // sps_temporal_id_nesting_flag

    if (maxSubLayersMinus1 !== 0) {
      return null;
    }

    // profile_tier_level(1, maxSubLayersMinus1) — only single temporal layer supported here
    const generalProfileSpace = br.readBits(2);
    const generalTierFlag = br.readBit();
    const generalProfileIdc = br.readBits(5);
    let compat = 0;
    for (let i = 0; i < 32; i++) compat = (compat << 1) | br.readBit();
    const constraint = new Uint8Array(6);
    for (let b = 0; b < 6; b++) {
      for (let bit = 7; bit >= 0; bit--) {
        constraint[b] |= br.readBit() << bit;
      }
    }
    const generalLevelIdc = br.readBits(8);

    return {
      generalProfileSpace,
      generalTierFlag,
      generalProfileIdc,
      generalProfileCompatibilityFlags: compat >>> 0,
      generalConstraintIndicatorFlags: constraint,
      generalLevelIdc,
    };
  } catch {
    return null;
  }
}

/** Track layout hint for tkhd/stsd; real display size comes from decoded frames. */
export function parseHevcSpsResolution(_spsNalWithHeader: Uint8Array): {
  width: number;
  height: number;
} {
  return { width: 1920, height: 1080 };
}

function buildHvcc(
  vps: Uint8Array,
  sps: Uint8Array,
  pps: Uint8Array,
  ptl: HevcPtlInfo
): Uint8Array {
  const nalArrays: { type: number; nals: Uint8Array[] }[] = [
    { type: 32, nals: [vps] },
    { type: 33, nals: [sps] },
    { type: 34, nals: [pps] },
  ];

  let bodySize = 23;
  for (const arr of nalArrays) {
    bodySize += 1 + 2;
    for (const nal of arr.nals) {
      bodySize += 2 + nal.length;
    }
  }

  const buf = new Uint8Array(bodySize);
  let o = 0;
  buf[o++] = 1;
  buf[o++] =    (ptl.generalProfileSpace << 6)
    | (ptl.generalTierFlag << 5)
    | ptl.generalProfileIdc;
  writeU32BE(buf, o, ptl.generalProfileCompatibilityFlags);
  o += 4;
  buf.set(ptl.generalConstraintIndicatorFlags, o);
  o += 6;
  buf[o++] = ptl.generalLevelIdc;
  // min_spatial_segmentation_idc(12)=0 + reserved(4)=0xF → 0xF0 0x00 (ISO 14496-15)
  buf[o++] = 0xf0;
  buf[o++] = 0x00;
  buf[o++] = 0xfc;
  buf[o++] = 0xfd;
  buf[o++] = 0xf8;
  buf[o++] = 0xf8;
  writeU16BE(buf, o, 0);
  o += 2;
  buf[o++] = 0x07;
  buf[o++] = nalArrays.length;

  for (const { type, nals } of nalArrays) {
    buf[o++] = (1 << 7) | type;
    writeU16BE(buf, o, nals.length);
    o += 2;
    for (const nal of nals) {
      writeU16BE(buf, o, nal.length);
      o += 2;
      buf.set(nal, o);
      o += nal.length;
    }
  }

  return box('hvcC', buf);
}

function hvc1SampleEntry(
  hvcc: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  const content = new Uint8Array(78 + hvcc.length);
  writeU16BE(content, 6, 1);
  writeU16BE(content, 24, width);
  writeU16BE(content, 26, height);
  writeU32BE(content, 28, 0x00480000);
  writeU32BE(content, 32, 0x00480000);
  writeU16BE(content, 40, 1);
  writeU16BE(content, 74, 0x0018);
  writeU16BE(content, 76, 0xffff);
  content.set(hvcc, 78);
  return box('hvc1', content);
}

function stsdHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  const hvc1Box = hvc1SampleEntry(hvcc, width, height);
  const content = concat(new Uint8Array([0, 0, 0, 1]), hvc1Box);
  return fullBox('stsd', 0, 0, content);
}

function stts(): Uint8Array {
  return fullBox('stts', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

function stsc(): Uint8Array {
  return fullBox('stsc', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

function stsz(): Uint8Array {
  return fullBox('stsz', 0, 0, new Uint8Array(12));
}

function stco(): Uint8Array {
  return fullBox('stco', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

function stblHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  return box(
    'stbl',
    stsdHevc(hvcc, width, height),
    stts(),
    stsc(),
    stsz(),
    stco()
  );
}

function dinf(): Uint8Array {
  const urlBox = fullBox('url ', 0, 1, new Uint8Array(0));
  const drefContent = concat(new Uint8Array([0, 0, 0, 1]), urlBox);
  return box('dinf', fullBox('dref', 0, 0, drefContent));
}

function vmhd(): Uint8Array {
  return fullBox('vmhd', 0, 1, new Uint8Array(8));
}

function minfHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  return box('minf', vmhd(), dinf(), stblHevc(hvcc, width, height));
}

function hdlr(): Uint8Array {
  const content = new Uint8Array(21);
  writeStr(content, 4, 'vide');
  return fullBox('hdlr', 0, 0, content);
}

function mdhd(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 8, 90000);
  writeU16BE(content, 16, 0x55c4);
  return fullBox('mdhd', 0, 0, content);
}

function mdiaHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  return box('mdia', mdhd(), hdlr(), minfHevc(hvcc, width, height));
}

function tkhd(width: number, height: number): Uint8Array {
  const content = new Uint8Array(80);
  writeU32BE(content, 8, 1);
  writeU32BE(content, 36, 0x00010000);
  writeU32BE(content, 52, 0x00010000);
  writeU32BE(content, 68, 0x40000000);
  writeU32BE(content, 72, width << 16);
  writeU32BE(content, 76, height << 16);
  return fullBox('tkhd', 0, 3, content);
}

function trakHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  return box('trak', tkhd(width, height), mdiaHevc(hvcc, width, height));
}

function mvhd(): Uint8Array {
  const content = new Uint8Array(96);
  writeU32BE(content, 8, 90000);
  writeU32BE(content, 16, 0x00010000);
  writeU16BE(content, 20, 0x0100);
  writeU32BE(content, 32, 0x00010000);
  writeU32BE(content, 48, 0x00010000);
  writeU32BE(content, 64, 0x40000000);
  writeU32BE(content, 92, 2);
  return fullBox('mvhd', 0, 0, content);
}

function trex(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 0, 1);
  writeU32BE(content, 4, 1);
  return fullBox('trex', 0, 0, content);
}

function mvex(): Uint8Array {
  return box('mvex', trex());
}

function moovHevc(hvcc: Uint8Array, width: number, height: number): Uint8Array {
  return box('moov', mvhd(), trakHevc(hvcc, width, height), mvex());
}

function ftypHevc(): Uint8Array {
  const content = new Uint8Array(24);
  writeStr(content, 0, 'iso6');
  writeU32BE(content, 4, 1);
  writeStr(content, 8, 'iso6');
  writeStr(content, 12, 'mp41');
  writeStr(content, 16, 'hvc1');
  writeStr(content, 20, 'hev1');
  return box('ftyp', content);
}

function mfhd(seq: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, seq);
  return fullBox('mfhd', 0, 0, content);
}

function tfhd(): Uint8Array {
  return fullBox('tfhd', 0, 0x20000, new Uint8Array([0, 0, 0, 1]));
}

function trun(
  duration: number,
  keyframe: boolean,
  offset: number,
  sampleSize: number,
  compositionTimeOffset: number = 0
): Uint8Array {
  const content = new Uint8Array(24);
  writeU32BE(content, 0, 1);
  writeU32BE(content, 4, offset);
  writeU32BE(content, 8, duration);
  writeU32BE(content, 12, sampleSize);
  const flags = keyframe ? 0x02000000 : 0x01010000;
  writeU32BE(content, 16, flags);
  writeU32BE(
    content,
    20,
    compositionTimeOffset < 0
      ? compositionTimeOffset >>> 0
      : compositionTimeOffset
  );
  return fullBox('trun', 1, 0xf01, content);
}

function tfdt(baseDecodeTime: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, baseDecodeTime);
  return fullBox('tfdt', 0, 0, content);
}

function traf(
  duration: number,
  keyframe: boolean,
  moofLen: number,
  baseDecodeTime: number,
  sampleSize: number,
  compositionTimeOffset: number = 0
): Uint8Array {
  return box(
    'traf',
    tfhd(),
    tfdt(baseDecodeTime),
    trun(duration, keyframe, moofLen + 8, sampleSize, compositionTimeOffset)
  );
}

function moof(
  seq: number,
  duration: number,
  keyframe: boolean,
  baseDecodeTime: number,
  sampleSize: number,
  compositionTimeOffset: number = 0
): Uint8Array {
  const mfhdBox = mfhd(seq);
  const trafBox = traf(
    duration,
    keyframe,
    100,
    baseDecodeTime,
    sampleSize,
    compositionTimeOffset
  );
  return box('moof', mfhdBox, trafBox);
}

function mdat(data: Uint8Array): Uint8Array {
  return box('mdat', data);
}

function pickMimeCodec(ptl: HevcPtlInfo): string | null {
  const MS = (window.ManagedMediaSource ?? window.MediaSource) as
    | typeof MediaSource
    | undefined;
  if (!MS?.isTypeSupported) return null;
  const compat = ptl.generalProfileCompatibilityFlags
    .toString(16)
    .padStart(8, '0');
  const tier = ptl.generalTierFlag ? 'H' : 'L';
  // Prefer hvc1.* first — stsd uses hvc1 sample entry; mismatched hev1 MIME can confuse some decoders.
  const candidates = [
    `hvc1.${ptl.generalProfileSpace}.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}.B0`,
    `hvc1.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}`,
    `hev1.${ptl.generalProfileSpace}.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}.B0`,
    `hev1.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}`,
  ];
  for (const c of candidates) {
    const mime = `video/mp4; codecs="${c}"`;
    if (MS.isTypeSupported(mime)) return mime;
  }
  return null;
}

export function createHevcInitSegment(
  vps: Uint8Array,
  sps: Uint8Array,
  pps: Uint8Array
): { segment: Uint8Array; mimeCodec: string } | null {
  const ptl = parseHevcSpsPtl(sps);
  if (!ptl) return null;
  const mimeCodec = pickMimeCodec(ptl);
  if (!mimeCodec) return null;
  const hvcc = buildHvcc(vps, sps, pps, ptl);
  const { width, height } = parseHevcSpsResolution(sps);
  return {
    segment: concat(ftypHevc(), moovHevc(hvcc, width, height)),
    mimeCodec,
  };
}

export class HevcMp4Muxer {
  private seq = 0;

  private duration = 3000;

  private baseDecodeTime = 0;

  setFrameRate(fps: number): void {
    const newDuration = 90000 / fps;
    if (Math.abs(newDuration - this.duration) > 100) {
      this.duration = newDuration;
    }
  }

  createMediaSegment(
    nal: Uint8Array,
    keyframe: boolean,
    timestamp90kHz?: number,
    compositionTimeOffset: number = 0
  ): Uint8Array {
    this.seq++;
    let decodeTime90kHz: number;
    if (timestamp90kHz !== undefined) {
      decodeTime90kHz = timestamp90kHz;
    } else {
      decodeTime90kHz = this.baseDecodeTime;
      this.baseDecodeTime += this.duration;
    }
    const moofBox = moof(
      this.seq,
      this.duration,
      keyframe,
      decodeTime90kHz,
      nal.length,
      compositionTimeOffset
    );
    const mdatBox = mdat(nal);
    return concat(moofBox, mdatBox);
  }
}
