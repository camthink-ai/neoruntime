// @ts-nocheck
/**
 * HEVC (H.265) fMP4 封装（hvcC/hvc1）+ SPS PTL/分辨率解析 + MIME 探测。
 * 移植自底座项目 hevcMp4.ts。VPS/SPS/PPS 需含 2 字节 HEVC NAL 头。
 */

import {
  box,
  concat,
  createInitSegmentBoxes,
  FragmentMuxerBase,
  ftyp,
  visualSampleEntry,
  writeU16BE,
  writeU32BE,
} from './mp4-boxes';

/** 去除 emulation prevention（00 00 03）得到 RBSP（跳过 2 字节 NAL 头）。 */
function rbspFromNalUnit(nalWithHeader: Uint8Array): Uint8Array {
  if (nalWithHeader.length <= 2) return new Uint8Array(0);
  const out: number[] = [];
  let i = 2;
  while (i < nalWithHeader.length) {
    if (
      i + 2 < nalWithHeader.length &&
      nalWithHeader[i] === 0 &&
      nalWithHeader[i + 1] === 0 &&
      nalWithHeader[i + 2] === 3
    ) {
      out.push(0, 0);
      i += 3;
    } else {
      out.push(nalWithHeader[i]);
      i += 1;
    }
  }
  return new Uint8Array(out);
}

export class BitReader {
  private bitPos = 0;

  constructor(private data: Uint8Array) {}

  readBit(): number {
    const byteIdx = this.bitPos >> 3;
    const bitIdx = 7 - (this.bitPos & 7);
    this.bitPos += 1;
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
    while (this.readBit() === 0 && zeros < 32) zeros += 1;
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

/** 解析 SPS 的 general profile_tier_level。 */
export function parseHevcSpsPtl(spsNalWithHeader: Uint8Array): HevcPtlInfo | null {
  try {
    const rbsp = rbspFromNalUnit(spsNalWithHeader);
    if (rbsp.length < 13) return null;
    const br = new BitReader(rbsp);
    br.readBits(4); // sps_video_parameter_set_id
    br.readBits(3); // sps_max_sub_layers_minus1
    br.readBit(); // sps_temporal_id_nesting_flag

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

/** RFC 6381 HEVC codec string from the stream's general profile_tier_level. */
export function hevcCodecString(ptl: HevcPtlInfo): string {
  let source = ptl.generalProfileCompatibilityFlags >>> 0;
  let reversed = 0;
  for (let i = 0; i < 32; i++) { reversed = ((reversed << 1) | (source & 1)) >>> 0; source >>>= 1; }
  const space = ['', 'A', 'B', 'C'][ptl.generalProfileSpace] ?? '';
  const tier = ptl.generalTierFlag ? 'H' : 'L';
  const constraints = Array.from(ptl.generalConstraintIndicatorFlags);
  while (constraints.length && constraints[constraints.length - 1] === 0) constraints.pop();
  const suffix = constraints.map(value => `.${value.toString(16).toUpperCase().padStart(2, '0')}`).join('');
  return `hvc1.${space}${ptl.generalProfileIdc}.${reversed.toString(16).toUpperCase()}.${tier}${ptl.generalLevelIdc}${suffix}`;
}

function skipProfileTierLevel(br: BitReader, maxSubLayersMinus1: number): void {
  br.readBits(96);
  if (maxSubLayersMinus1 <= 0) return;
  const subLayerProfilePresent: boolean[] = [];
  const subLayerLevelPresent: boolean[] = [];
  for (let i = 0; i < maxSubLayersMinus1; i++) {
    subLayerProfilePresent.push(br.readBit() === 1);
    subLayerLevelPresent.push(br.readBit() === 1);
  }
  br.readBits(2 * (8 - maxSubLayersMinus1));
  for (let i = 0; i < maxSubLayersMinus1; i++) {
    if (subLayerProfilePresent[i]) br.readBits(88);
    if (subLayerLevelPresent[i]) br.readBits(8);
  }
}

/** 可见分辨率（含 conformance window 裁剪）；解析失败返回 null（猜尺寸比失败更糟）。 */
export function parseHevcSpsResolution(
  spsNalWithHeader: Uint8Array
): { width: number; height: number } | null {
  try {
    const rbsp = rbspFromNalUnit(spsNalWithHeader);
    if (rbsp.length < 12) return null;
    const br = new BitReader(rbsp);
    br.readBits(4);
    const maxSubLayersMinus1 = br.readBits(3);
    br.readBit();
    skipProfileTierLevel(br, maxSubLayersMinus1);
    br.readUE(); // sps_seq_parameter_set_id
    const chromaFormatIdc = br.readUE();
    const separateColourPlaneFlag = chromaFormatIdc === 3 ? br.readBit() : 0;
    const codedWidth = br.readUE();
    const codedHeight = br.readUE();
    if (codedWidth <= 0 || codedHeight <= 0) return null;
    let cropLeft = 0;
    let cropRight = 0;
    let cropTop = 0;
    let cropBottom = 0;
    if (br.readBit()) {
      cropLeft = br.readUE();
      cropRight = br.readUE();
      cropTop = br.readUE();
      cropBottom = br.readUE();
    }
    const chromaArrayType = separateColourPlaneFlag ? 0 : chromaFormatIdc;
    const subWidthC = chromaArrayType === 1 || chromaArrayType === 2 ? 2 : 1;
    const subHeightC = chromaArrayType === 1 ? 2 : 1;
    const width = codedWidth - (cropLeft + cropRight) * subWidthC;
    const height = codedHeight - (cropTop + cropBottom) * subHeightC;
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return null;
    return { width, height };
  } catch {
    return null;
  }
}

function buildHvcc(vps: Uint8Array, sps: Uint8Array, pps: Uint8Array, ptl: HevcPtlInfo): Uint8Array {
  const nalArrays: { type: number; nals: Uint8Array[] }[] = [
    { type: 32, nals: [vps] },
    { type: 33, nals: [sps] },
    { type: 34, nals: [pps] },
  ];
  let bodySize = 23;
  for (const arr of nalArrays) {
    bodySize += 1 + 2;
    for (const nal of arr.nals) bodySize += 2 + nal.length;
  }
  const buf = new Uint8Array(bodySize);
  let o = 0;
  buf[o++] = 1; // configurationVersion
  buf[o++] =
    (ptl.generalProfileSpace << 6) | (ptl.generalTierFlag << 5) | ptl.generalProfileIdc;
  writeU32BE(buf, o, ptl.generalProfileCompatibilityFlags);
  o += 4;
  buf.set(ptl.generalConstraintIndicatorFlags, o);
  o += 6;
  buf[o++] = ptl.generalLevelIdc;
  buf[o++] = 0xf0; // min_spatial_segmentation_idc(12)=0 + reserved(4)=0xF
  buf[o++] = 0x00;
  buf[o++] = 0xfc;
  buf[o++] = 0xfd;
  buf[o++] = 0xf8;
  buf[o++] = 0xf8;
  writeU16BE(buf, o, 0);
  o += 2;
  buf[o++] = 0x07; // lengthSizeMinusOne = 3
  buf[o++] = nalArrays.length;
  for (const { type, nals } of nalArrays) {
    buf[o++] = (1 << 7) | type; // array_completeness=1
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

/**
 * 从真实 SPS PTL 生成候选 MIME 并用 isTypeSupported 探测。
 * hvc1 优先（stsd 使用 hvc1 sample entry，hev1 MIME 与之错配会困扰部分解码器）。
 */
export function pickHevcMimeCodec(ptl: HevcPtlInfo): string | null {
  if (typeof window === 'undefined') return null;
  const w = window as unknown as {
    ManagedMediaSource?: typeof MediaSource;
    MediaSource?: typeof MediaSource;
  };
  const MS = w.ManagedMediaSource ?? w.MediaSource;
  if (!MS?.isTypeSupported) return null;
  const compat = ptl.generalProfileCompatibilityFlags.toString(16).padStart(8, '0');
  const tier = ptl.generalTierFlag ? 'H' : 'L';
  const candidates = [
    `hvc1.${ptl.generalProfileSpace}.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}.B0`,
    `hvc1.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}`,
    `hev1.${ptl.generalProfileSpace}.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}.B0`,
    `hev1.${ptl.generalProfileIdc}.${compat}.${tier}${ptl.generalLevelIdc}`,
  ];
  for (const c of candidates) {
    const mime = `video/mp4; codecs="${c}"`;
    try {
      if (MS.isTypeSupported(mime)) return mime;
    } catch {
      /* try next */
    }
  }
  return null;
}

export interface HevcInitResult {
  segment: Uint8Array;
  mimeCodec: string;
}

/** VPS/SPS/PPS 齐 → init segment + 探测通过的 MIME；任一环节失败返回 null（→ 码流级降级信号）。 */
export function createHevcInitSegment(
  vps: Uint8Array,
  sps: Uint8Array,
  pps: Uint8Array
): HevcInitResult | null {
  const ptl = parseHevcSpsPtl(sps);
  if (!ptl) return null;
  const mimeCodec = pickHevcMimeCodec(ptl);
  if (!mimeCodec) return null;
  const dims = parseHevcSpsResolution(sps);
  if (!dims) return null;
  const hvcc = buildHvcc(vps, sps, pps, ptl);
  const entry = visualSampleEntry('hvc1', hvcc, dims.width, dims.height);
  const segment = concat(
    createInitSegmentBoxes(ftyp('iso6', 1, ['iso6', 'mp41', 'hvc1', 'hev1']), entry, dims.width, dims.height)
  );
  return { segment, mimeCodec };
}

export class HevcMuxer extends FragmentMuxerBase {}
