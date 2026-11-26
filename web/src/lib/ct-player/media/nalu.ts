/**
 * NALU 工具：扫描、codec 嗅探、关键帧判定、AnnexB↔AVCC 转换。
 * 全库只在这里扫描 start code（P1：解析一次，处处复用）。
 */

import type { EncodedFrame, VideoCodec } from '../shared/types';

export const H264_NAL_TYPE = {
  SLICE: 1,
  IDR: 5,
  SEI: 6,
  SPS: 7,
  PPS: 8,
  AUD: 9,
} as const;

export const HEVC_NAL_TYPE = {
  VPS: 32,
  SPS: 33,
  PPS: 34,
  IDR_W_RADL: 19,
  IDR_N_LP: 20,
} as const;

/** HEVC IRAP（BLA/IDR/CRA，16..23）均视为随机访问点。 */
export function isHevcRap(type: number): boolean {
  return type >= 16 && type <= 23;
}

export function isH264Vcl(type: number): boolean {
  return type >= 1 && type <= 5;
}

export function isHevcVcl(type: number): boolean {
  return (type >= 0 && type <= 9) || (type >= 16 && type <= 21);
}

/** 扫描结果：一条 NALU 的定位信息（兼容 3/4 字节起始码）。 */
export interface ScannedNalu {
  /** 起始码首字节偏移 */
  start: number;
  /** 起始码长度（3 或 4） */
  codeLen: number;
  /** NALU 头部字节偏移（不含起始码） */
  header: number;
  /** 到下一条起始码（或缓冲尾）为止的负载长度 */
  payloadLen: number;
  /** h264: header&0x1f；需结合 codec 语境解释 */
  rawHeader: number;
  /** 距缓冲尾是否完整收到（true=被截断） */
  partial: boolean;
}

function findStartCode(data: Uint8Array, from: number): { pos: number; codeLen: number } | null {
  for (let i = from; i + 3 <= data.length; i++) {
    // 先判 4 字节：00 00 00 01 在 3 字节检测下会被误报在 i+1 处
    if (i + 4 <= data.length && data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 0 && data[i + 3] === 1) {
      return { pos: i, codeLen: 4 };
    }
    if (data[i] === 0 && data[i + 1] === 0 && data[i + 2] === 1) {
      return { pos: i, codeLen: 3 };
    }
  }
  return null;
}

/**
 * 扫描 AnnexB 缓冲中的所有 NALU（兼容 3/4 字节起始码）。
 * 最后一条若没有后续起始码界定，标记 partial=true（可能被传输截断），
 * 由调用者决定保留拼接还是按完整处理。
 */
export function scanAnnexBNalus(data: Uint8Array): ScannedNalu[] {
  const out: ScannedNalu[] = [];
  let cur = findStartCode(data, 0);
  while (cur) {
    const header = cur.pos + cur.codeLen;
    const next = findStartCode(data, header);
    let end = data.length;
    let partial = true;
    if (next) {
      end = next.pos;
      // 起始码前方的连续零属于起始码前导（有效 RBSP 不会以 00 00 结尾）
      while (end > header && data[end - 1] === 0) end--;
      partial = false;
    }
    out.push({
      start: cur.pos,
      codeLen: cur.codeLen,
      header,
      payloadLen: Math.max(0, end - header),
      rawHeader: data[header] ?? 0,
      partial,
    });
    cur = next;
  }
  return out;
}

/**
 * 单条 NALU 头部的 codec 提示（移植自底座项目 sniffCodecIfUnknown）。
 * SPS/PPS/VPS 字节特征明确；切片 NALU 有歧义，返回 null 由上层沿用已知状态。
 */
export function nalCodecHint(rawHeader: number): VideoCodec | null {
  if (rawHeader & 0x80) return null; // forbidden_zero_bit 必须为 0
  const h264t = rawHeader & 0x1f;
  const h264Ref = (rawHeader >> 5) & 0x3;
  const hevcT = (rawHeader >> 1) & 0x3f;
  // h264 参数集：type 7/8 且 ref_idc>0。头字节不一定是 0x67/0x68——
  // 实测设备（WSFS 协议）发 0x27/0x28（ref_idc=1），必须按类型语义判定。
  // h265 参数集头字节 0x40/0x42/0x44 的低 5 位是 0/2/4，与 type 7/8 不冲突。
  if (h264Ref > 0 && (h264t === 7 || h264t === 8)) return 'h264';
  if (hevcT >= 32 && hevcT <= 34) {
    // 6 比特对齐特征强于 5 比特歧义（h265 参数集优先判定）
    if (rawHeader === 0x40 || rawHeader === 0x42 || rawHeader === 0x44) return 'h265';
    if (h264t >= 1 && h264t <= 5) return null; // 切片：歧义，交由连续性
    if (h264t === 0) return 'h265';
  }
  return null;
}

/**
 * AVCC 单 NAL 判定：首 4 字节大端长度恰好等于剩余负载（底座项目同款协议约定）。
 */
export function isSingleAvccNalu(data: Uint8Array): boolean {
  if (data.length < 5) return false;
  const first =
    ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) >>> 0;
  return first > 0 && first + 4 === data.length;
}

/** AnnexB 帧 → AVCC 字节流（4 字节长度前缀）。基于 nalus 索引，不再扫描。 */
export function frameToAvcc(frame: EncodedFrame, filter?: (type: number) => boolean): Uint8Array {
  const selected: number[] = [];
  let total = 0;
  for (let i = 0; i < frame.nalus.length; i++) {
    const n = frame.nalus[i];
    const payloadLen = n.size - 4;
    if (payloadLen <= 0) continue;
    if (filter && !filter(n.type)) continue;
    selected.push(i);
    total += 4 + payloadLen;
  }
  const out = new Uint8Array(total);
  let off = 0;
  for (const i of selected) {
    const n = frame.nalus[i];
    const payloadLen = n.size - 4;
    out[off] = (payloadLen >>> 24) & 0xff;
    out[off + 1] = (payloadLen >>> 16) & 0xff;
    out[off + 2] = (payloadLen >>> 8) & 0xff;
    out[off + 3] = payloadLen & 0xff;
    out.set(frame.data.subarray(n.offset + 4, n.offset + 4 + payloadLen), off + 4);
    off += 4 + payloadLen;
  }
  return out;
}

/** 取第 i 条 NALU 的负载（不含起始码）副本。 */
export function naluPayload(frame: EncodedFrame, i: number): Uint8Array {
  const n = frame.nalus[i];
  if (!n) return new Uint8Array(0);
  return new Uint8Array(frame.data.subarray(n.offset + 4, n.offset + n.size));
}

export function bytesEqual(a: Uint8Array, b: Uint8Array): boolean {
  if (a.length !== b.length) return false;
  for (let i = 0; i < a.length; i++) {
    if (a[i] !== b[i]) return false;
  }
  return true;
}
