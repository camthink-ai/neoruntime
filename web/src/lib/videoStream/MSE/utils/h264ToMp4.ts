/**
 * Simple H264 to MP4 Fragment Builder for MSE
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

// Create avcC from SPS and PPS
function avcC(sps: Uint8Array, pps: Uint8Array): Uint8Array {
  const content = new Uint8Array(11 + sps.length + pps.length);
  let i = 0;
  content[i++] = 1; // configurationVersion
  const [, profileIdc, profileCompatibility, levelIdc] = sps;
  content[i++] = profileIdc; // profile_idc
  content[i++] = profileCompatibility; // profile_compatibility
  content[i++] = levelIdc; // level_idc
  content[i++] = 0xff; // lengthSizeMinusOne = 3 (4 bytes)
  content[i++] = 0xe1; // reserved + numOfSequenceParameterSets = 1
  content[i++] = (sps.length >> 8) & 0xff; // SPS length high byte (uint16BE)
  content[i++] = sps.length & 0xff; // SPS length low byte
  content.set(sps, i);
  i += sps.length;
  content[i++] = 1; // numOfPictureParameterSets = 1
  content[i++] = (pps.length >> 8) & 0xff; // PPS length high byte (uint16BE)
  content[i++] = pps.length & 0xff; // PPS length low byte
  content.set(pps, i);
  return box('avcC', content);
}

// Create avc1 sample entry
function avc1(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  const avcCBox = avcC(sps, pps);

  // Visual sample entry (simplified)
  const content = new Uint8Array(78 + avcCBox.length);

  // Reserved (6 bytes) + data_reference_index
  writeU16BE(content, 6, 1);
  // Pre_defined (16 bytes) - skip
  // Width (2 bytes)
  writeU16BE(content, 24, width);
  // Height (2 bytes)
  writeU16BE(content, 26, height);
  // Horizontal resolution (72.0)
  writeU32BE(content, 28, 0x00480000);
  // Vertical resolution
  writeU32BE(content, 32, 0x00480000);
  // Reserved (4 bytes)
  // Frame count
  writeU16BE(content, 40, 1);
  // Compressor name (32 bytes) - skip
  // Depth
  writeU16BE(content, 74, 0x0018);
  // Pre_defined
  writeU16BE(content, 76, 0xffff);
  // avcC
  content.set(avcCBox, 78);

  return box('avc1', content);
}

// Create stsd
function stsd(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  const avc1Box = avc1(sps, pps, width, height);
  const content = concat(
    new Uint8Array([0, 0, 0, 1]), // entry_count=1
    avc1Box
  );
  return fullBox('stsd', 0, 0, content);
}

// Create stts (empty)
function stts(): Uint8Array {
  return fullBox('stts', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

// Create stsc (empty)
function stsc(): Uint8Array {
  return fullBox('stsc', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

// Create stsz (empty)
function stsz(): Uint8Array {
  return fullBox('stsz', 0, 0, new Uint8Array(12));
}

// Create stco (empty)
function stco(): Uint8Array {
  return fullBox('stco', 0, 0, new Uint8Array([0, 0, 0, 0]));
}

// Create stbl
function stbl(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  return box(
    'stbl',
    stsd(sps, pps, width, height),
    stts(),
    stsc(),
    stsz(),
    stco()
  );
}

// Create dinf
function dinf(): Uint8Array {
  const urlBox = fullBox('url ', 0, 1, new Uint8Array(0));
  const drefContent = concat(
    new Uint8Array([0, 0, 0, 1]), // entry_count
    urlBox
  );
  return box('dinf', fullBox('dref', 0, 0, drefContent));
}

// Create vmhd
function vmhd(): Uint8Array {
  const content = new Uint8Array(8);
  return fullBox('vmhd', 0, 1, content);
}

// Create minf
function minf(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  return box('minf', vmhd(), dinf(), stbl(sps, pps, width, height));
}

// Create hdlr
function hdlr(): Uint8Array {
  const content = new Uint8Array(21);
  // pre_defined [0-3] = 0
  writeStr(content, 4, 'vide'); // handler_type
  // reserved [8-19] = 0, name [20] = '\0'
  return fullBox('hdlr', 0, 0, content);
}

// Create mdhd
function mdhd(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 8, 90000); // timescale (90kHz for H264)
  writeU16BE(content, 16, 0x55c4); // language
  return fullBox('mdhd', 0, 0, content);
}

// Create mdia
function mdia(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  return box('mdia', mdhd(), hdlr(), minf(sps, pps, width, height));
}

// Create tkhd (version 0, flags=3, 80 bytes content)
function tkhd(width: number, height: number): Uint8Array {
  const content = new Uint8Array(80);
  // creation_time [0-3] = 0, modification_time [4-7] = 0
  writeU32BE(content, 8, 1); // track_ID
  // reserved [12-15] = 0, duration [16-19] = 0
  // reserved [20-27] = 0, layer [28-29] = 0, alternate_group [30-31] = 0
  // volume [32-33] = 0 (video track), reserved [34-35] = 0
  // matrix (identity) at offset 36
  writeU32BE(content, 36, 0x00010000);
  writeU32BE(content, 52, 0x00010000);
  writeU32BE(content, 68, 0x40000000);
  // width/height (16.16 fixed point)
  writeU32BE(content, 72, width << 16);
  writeU32BE(content, 76, height << 16);
  return fullBox('tkhd', 0, 3, content);
}

// Create trak
function trak(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  return box('trak', tkhd(width, height), mdia(sps, pps, width, height));
}

// Create mvhd (version 0, 96 bytes content)
function mvhd(): Uint8Array {
  const content = new Uint8Array(96);
  // creation_time [0-3] = 0, modification_time [4-7] = 0
  writeU32BE(content, 8, 90000); // timescale (90kHz for H264)
  // duration [12-15] = 0 (live/fragmented)
  writeU32BE(content, 16, 0x00010000); // rate = 1.0
  writeU16BE(content, 20, 0x0100); // volume = 1.0
  // reserved [22-31] = 0
  // matrix (identity) at offset 32
  writeU32BE(content, 32, 0x00010000);
  writeU32BE(content, 48, 0x00010000);
  writeU32BE(content, 64, 0x40000000);
  // pre_defined [68-91] = 0
  writeU32BE(content, 92, 2); // next_track_ID
  return fullBox('mvhd', 0, 0, content);
}

// Create trex (Track Extends - required for fragmented MP4)
function trex(): Uint8Array {
  const content = new Uint8Array(20);
  writeU32BE(content, 0, 1); // track_ID
  writeU32BE(content, 4, 1); // default_sample_description_index
  return fullBox('trex', 0, 0, content);
}

// Create mvex (Movie Extends - signals fragmented MP4)
function mvex(): Uint8Array {
  return box('mvex', trex());
}

// Create moov
function moov(
  sps: Uint8Array,
  pps: Uint8Array,
  width: number,
  height: number
): Uint8Array {
  return box('moov', mvhd(), trak(sps, pps, width, height), mvex());
}

// Create ftyp
function ftyp(): Uint8Array {
  const content = new Uint8Array(20);
  writeStr(content, 0, 'isom');
  writeU32BE(content, 4, 512);
  writeStr(content, 8, 'isom');
  writeStr(content, 12, 'iso2');
  writeStr(content, 16, 'avc1');
  return box('ftyp', content);
}

// Create mfhd
function mfhd(seq: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, seq);
  return fullBox('mfhd', 0, 0, content);
}

// Create tfhd
function tfhd(): Uint8Array {
  return fullBox('tfhd', 0, 0x20000, new Uint8Array([0, 0, 0, 1])); // track_ID=1
}

// Create trun with composition time offset support for B-frames
function trun(
  duration: number,
  keyframe: boolean,
  offset: number,
  sampleSize: number,
  compositionTimeOffset: number = 0
): Uint8Array {
  // flags: data-offset(0x1) | sample-duration(0x100) | sample-size(0x200) | sample-flags(0x400) | sample-composition-time-offsets(0x800)
  const content = new Uint8Array(24); // 20 bytes + 4 bytes for CTO
  writeU32BE(content, 0, 1); // sample_count
  writeU32BE(content, 4, offset); // data_offset
  writeU32BE(content, 8, duration); // sample_duration
  writeU32BE(content, 12, sampleSize); // sample_size
  const flags = keyframe ? 0x02000000 : 0x01010000;
  writeU32BE(content, 16, flags); // sample_flags

  // CTO (Composition Time Offset) is signed 32-bit integer
  // For B-frames, CTO may be negative (PTS < DTS), need to handle as signed
  if (compositionTimeOffset < 0) {
    // Negative CTO: convert to unsigned 32-bit (two's complement)
    const unsignedOffset = compositionTimeOffset >>> 0; // >>> converts to unsigned
    writeU32BE(content, 20, unsignedOffset);
  } else {
    writeU32BE(content, 20, compositionTimeOffset);
  }

  // version=1 so composition_time_offset is interpreted as signed int32
  // (required for B-frames where PTS < DTS → negative CTO).
  return fullBox('trun', 1, 0xf01, content); // flags |= 0x800 for CTO
}

// Create tfdt (Track Fragment Decode Time)
function tfdt(baseDecodeTime: number): Uint8Array {
  const content = new Uint8Array(4);
  writeU32BE(content, 0, baseDecodeTime);
  return fullBox('tfdt', 0, 0, content);
}

// Create traf with composition time offset
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

// Create moof
// trun: 8(box) + 4(ver+flags) + 24(content) = 36 bytes (with CTO)
// traf: 8(box) + 16(tfhd) + 16(tfdt) + 36(trun) = 76 bytes
// moof: 8(box) + 16(mfhd) + 76(traf) = 100 bytes
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

// Create mdat
function mdat(data: Uint8Array): Uint8Array {
  return box('mdat', data);
}

/**
 * MP4Muxer
 */
export class MP4Muxer {
  private seq = 0;

  private duration = 3000; // Duration in 90kHz timescale (33.333ms for 30fps)

  private baseDecodeTime = 0;

  private width = 1920;

  private height = 1080;

  createInitSegment(
    sps: Uint8Array,
    pps: Uint8Array,
    width?: number,
    height?: number
  ): Uint8Array {
    if (width) this.width = width;
    if (height) this.height = height;

    return concat(ftyp(), moov(sps, pps, this.width, this.height));
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
      // Use backend timestamp directly without smoothing
      // Smoothing causes timestamp discontinuity which breaks P-frame decoding for GOP>1
      decodeTime90kHz = timestamp90kHz;
    } else {
      // Fallback: use counter-based timing
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

  setFrameRate(fps: number): void {
    // Calculate duration in 90kHz timescale
    const newDuration = 90000 / fps;
    // Update if fps changed by more than ~1fps (100 ticks ≈ 1ms at 90kHz)
    if (Math.abs(newDuration - this.duration) > 100) {
      this.duration = newDuration;
    }
  }
}
