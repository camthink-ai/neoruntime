/**
 * ISO-BMFF box 构建器（fMP4 remuxer 共用）。
 * 字节布局移植自底座项目（经真机 MSE 验证的布局，勿随意改动）。
 */

export function writeU32BE(arr: Uint8Array, offset: number, value: number): void {
  arr[offset] = (value >>> 24) & 0xff;
  arr[offset + 1] = (value >>> 16) & 0xff;
  arr[offset + 2] = (value >>> 8) & 0xff;
  arr[offset + 3] = value & 0xff;
}

export function writeU16BE(arr: Uint8Array, offset: number, value: number): void {
  arr[offset] = (value >>> 8) & 0xff;
  arr[offset + 1] = value & 0xff;
}

function writeStr(arr: Uint8Array, offset: number, str: string): void {
  for (let i = 0; i < str.length; i++) {
    arr[offset + i] = str.charCodeAt(i);
  }
}

export function box(type: string, ...children: Uint8Array[]): Uint8Array {
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

export function fullBox(type: string, version: number, flags: number, content: Uint8Array): Uint8Array {
  const full = new Uint8Array(4 + content.length);
  full[0] = version;
  full[1] = (flags >>> 16) & 0xff;
  full[2] = (flags >>> 8) & 0xff;
  full[3] = flags & 0xff;
  full.set(content, 4);
  return box(type, full);
}

export function concat(...arrays: Uint8Array[]): Uint8Array {
  const total = arrays.reduce((s, a) => s + a.length, 0);
  const result = new Uint8Array(total);
  let offset = 0;
  for (const arr of arrays) {
    result.set(arr, offset);
    offset += arr.length;
  }
  return result;
}

export function ftyp(major: string, minor: number, compatible: string[]): Uint8Array {
  const content = new Uint8Array(4 + 4 + compatible.length * 4);
  writeStr(content, 0, major);
  writeU32BE(content, 4, minor);
  compatible.forEach((brand, i) => writeStr(content, 8 + i * 4, brand));
  return box('ftyp', content);
}

// ---- init segment 公共结构（avc1/hvc1 仅 sample entry 不同） ----

function stsd(sampleEntry: Uint8Array): Uint8Array {
  return fullBox('stsd', 0, 0, concat(new Uint8Array([0, 0, 0, 1]), sampleEntry));
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

function dinf(): Uint8Array {
  const url = fullBox('url ', 0, 1, new Uint8Array(0));
  return box('dinf', fullBox('dref', 0, 0, concat(new Uint8Array([0, 0, 0, 1]), url)));
}

function vmhd(): Uint8Array {
  return fullBox('vmhd', 0, 1, new Uint8Array(8));
}

function hdlr(): Uint8Array {
  const content = new Uint8Array(21);
  writeStr(content, 4, 'vide');
  return fullBox('hdlr', 0, 0, content);
}

function mdhd(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 8, 90000); // timescale 90kHz
  writeU16BE(content, 16, 0x55c4);
  return fullBox('mdhd', 0, 0, content);
}

function tkhd(width: number, height: number): Uint8Array {
  const content = new Uint8Array(80);
  writeU32BE(content, 8, 1); // track_ID
  writeU32BE(content, 36, 0x00010000); // matrix
  writeU32BE(content, 52, 0x00010000);
  writeU32BE(content, 68, 0x40000000);
  writeU32BE(content, 72, width << 16);
  writeU32BE(content, 76, height << 16);
  return fullBox('tkhd', 0, 3, content);
}

function mvhd(): Uint8Array {
  const content = new Uint8Array(96);
  writeU32BE(content, 8, 90000);
  writeU32BE(content, 16, 0x00010000); // rate 1.0
  writeU16BE(content, 20, 0x0100); // volume 1.0
  writeU32BE(content, 32, 0x00010000);
  writeU32BE(content, 48, 0x00010000);
  writeU32BE(content, 64, 0x40000000);
  writeU32BE(content, 92, 2); // next_track_ID
  return fullBox('mvhd', 0, 0, content);
}

function trex(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 0, 1); // track_ID
  writeU32BE(content, 4, 1); // default_sample_description_index
  return fullBox('trex', 0, 0, content);
}

/** Visual sample entry（avc1/hvc1 共用骨架，78 字节头 + 子 box）。 */
export function visualSampleEntry(type: string, codecBox: Uint8Array, width: number, height: number): Uint8Array {
  const content = new Uint8Array(78 + codecBox.length);
  writeU16BE(content, 6, 1); // data_reference_index
  writeU16BE(content, 24, width);
  writeU16BE(content, 26, height);
  writeU32BE(content, 28, 0x00480000); // 72 dpi
  writeU32BE(content, 32, 0x00480000);
  writeU16BE(content, 40, 1); // frame_count
  writeU16BE(content, 74, 0x0018); // depth
  writeU16BE(content, 76, 0xffff);
  content.set(codecBox, 78);
  return box(type, content);
}

export function createInitSegmentBoxes(
  ftypBox: Uint8Array,
  sampleEntry: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  const stbl = box('stbl', stsd(sampleEntry), stts(), stsc(), stsz(), stco());
  const minf = box('minf', vmhd(), dinf(), stbl);
  const mdia = box('mdia', mdhd(), hdlr(), minf);
  const trak = box('trak', tkhd(width, height), mdia);
  const moovBox = box('moov', mvhd(), trak, box('mvex', trex()));
  return concat(ftypBox, moovBox);
}

// ---- media segment（moof + mdat，两 codec 完全一致） ----

function mfhd(seq: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, seq);
  return fullBox('mfhd', 0, 0, content);
}

function tfhd(): Uint8Array {
  return fullBox('tfhd', 0, 0x20000, new Uint8Array([0, 0, 0, 1]));
}

function tfdt(baseDecodeTime: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, baseDecodeTime);
  return fullBox('tfdt', 0, 0, content);
}

function trun(duration: number, keyframe: boolean, offset: number, sampleSize: number, cto: number): Uint8Array {
  const content = new Uint8Array(24);
  writeU32BE(content, 0, 1); // sample_count
  writeU32BE(content, 4, offset); // data_offset
  writeU32BE(content, 8, duration);
  writeU32BE(content, 12, sampleSize);
  // key: non-sync sample 前导结构与底座一致
  writeU32BE(content, 16, keyframe ? 0x02000000 : 0x01010000);
  // CTO 为有符号 int32（B 帧 PTS<DTS 为负，二进制补码承载），trun v1 解释为 signed
  writeU32BE(content, 20, cto < 0 ? cto >>> 0 : cto);
  return fullBox('trun', 1, 0xf01, content);
}

/** moof 布局：8 + 16(mfhd) + 76(traf) = 100 字节（data_offset 依赖此常量）。 */
const MOOF_LEN = 100;

export function createMediaSegmentBoxes(
  seq: number,
  duration: number,
  keyframe: boolean,
  baseDecodeTime: number,
  sample: Uint8Array,
  cto: number
): Uint8Array {
  const traf = box(
    'traf',
    tfhd(),
    tfdt(baseDecodeTime),
    trun(duration, keyframe, MOOF_LEN + 8, sample.length, cto)
  );
  const moofBox = box('moof', mfhd(seq), traf);
  return concat(moofBox, box('mdat', sample));
}

/** 分片 muxer 公共状态机（seq/duration/baseDecodeTime/时间戳策略）。 */
export abstract class FragmentMuxerBase {
  protected seq = 0;
  protected duration = 3000; // 90kHz，30fps 默认
  private baseDecodeTime = 0;

  setFrameRate(fps: number): void {
    const next = 90000 / fps;
    // 变化小于 ~1ms 忽略，避免频繁重写
    if (Math.abs(next - this.duration) > 100) {
      this.duration = next;
    }
  }

  get segmentDuration(): number {
    return this.duration;
  }

  createMediaSegment(
    sample: Uint8Array,
    keyframe: boolean,
    timestamp90kHz?: number,
    cto = 0
  ): Uint8Array {
    this.seq += 1;
    let decodeTime: number;
    if (timestamp90kHz !== undefined) {
      // 直接使用时间轴：平滑会制造断档，破坏 GOP>1 的 P 帧解码（底座结论）
      decodeTime = timestamp90kHz;
    } else {
      decodeTime = this.baseDecodeTime;
      this.baseDecodeTime += this.duration;
    }
    return createMediaSegmentBoxes(this.seq, this.duration, keyframe, decodeTime, sample, cto);
  }
}
