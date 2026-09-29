import { describe, it, expect, beforeAll, vi } from 'vitest';
import {
  parseHevcSpsResolution,
  createHevcInitSegment,
} from '@/lib/videoStream/MSE/utils/hevcMp4';

class BitWriter {
  private bytes: number[] = [];

  private bitCount = 0;

  writeBits(value: number, n: number): void {
    for (let i = n - 1; i >= 0; i--) {
      const byteIdx = this.bitCount >> 3;
      if (byteIdx >= this.bytes.length) this.bytes.push(0);
      if (((value >>> i) & 1) === 1) {
        this.bytes[byteIdx] |= 1 << (7 - (this.bitCount & 7));
      }
      this.bitCount++;
    }
  }

  writeUE(value: number): void {
    const codeNum = value + 1;
    const len = 32 - Math.clz32(codeNum);
    this.writeBits(0, len - 1);
    this.writeBits(codeNum, len);
  }

  rbsp(): Uint8Array {
    return new Uint8Array(this.bytes);
  }
}

interface SpsOptions {
  width: number;
  height: number;
  chromaFormatIdc?: number;
  crop?: { left?: number; right?: number; top?: number; bottom?: number };
  maxSubLayersMinus1?: number;
  emulationPrevention?: boolean;
}

/** Build an HEVC SPS NAL (Annex-B style: 2-byte NAL header + RBSP). */
function buildSps(opts: SpsOptions): Uint8Array {
  const chromaFormatIdc = opts.chromaFormatIdc ?? 1;
  const maxSubLayersMinus1 = opts.maxSubLayersMinus1 ?? 0;
  const w = new BitWriter();
  w.writeBits(0, 4); // sps_video_parameter_set_id
  w.writeBits(maxSubLayersMinus1, 3);
  w.writeBits(1, 1); // sps_temporal_id_nesting_flag
  // profile_tier_level(1, maxSubLayersMinus1) — Main profile, level 120
  w.writeBits(0, 2); // general_profile_space
  w.writeBits(0, 1); // general_tier_flag
  w.writeBits(1, 5); // general_profile_idc
  w.writeBits(0x60000000, 32); // general_profile_compatibility_flags
  w.writeBits(0, 48); // general constraint indicator flags
  w.writeBits(120, 8); // general_level_idc
  for (let i = 0; i < maxSubLayersMinus1; i++) {
    w.writeBits(0, 1); // sub_layer_profile_present_flag[i]
    w.writeBits(0, 1); // sub_layer_level_present_flag[i]
  }
  if (maxSubLayersMinus1 > 0) {
    w.writeBits(0, 2 * (8 - maxSubLayersMinus1)); // reserved_zero_2bits
  }
  w.writeUE(0); // sps_seq_parameter_set_id
  w.writeUE(chromaFormatIdc);
  if (chromaFormatIdc === 3) w.writeBits(0, 1); // separate_colour_plane_flag
  w.writeUE(opts.width);
  w.writeUE(opts.height);
  const { crop } = opts;
  if (crop && (crop.left || crop.right || crop.top || crop.bottom)) {
    w.writeBits(1, 1); // conformance_window_flag
    w.writeUE(crop.left ?? 0);
    w.writeUE(crop.right ?? 0);
    w.writeUE(crop.top ?? 0);
    w.writeUE(crop.bottom ?? 0);
  } else {
    w.writeBits(0, 1); // conformance_window_flag
  }
  w.writeUE(0); // bit_depth_luma_minus8
  w.writeUE(0); // bit_depth_chroma_minus8
  w.writeUE(4); // log2_max_pic_order_cnt_lsb_minus4
  w.writeBits(1, 1); // rbsp_stop_one_bit (align to byte with zeros)

  let rbsp = w.rbsp();
  if (opts.emulationPrevention) {
    const ep: number[] = [];
    let zeros = 0;
    for (const byte of rbsp) {
      if (zeros === 2 && byte <= 3) {
        ep.push(0x03);
        zeros = 0;
      }
      if (byte === 0) zeros++;
      else zeros = 0;
      ep.push(byte);
    }
    rbsp = new Uint8Array(ep);
  }
  // NAL header: type 33 (SPS), nuh_layer_id 0, temporal_id_plus1 1
  return new Uint8Array([0x42, 0x01, ...rbsp]);
}

describe('parseHevcSpsResolution', () => {
  it('parses common resolutions without a conformance window', () => {
    expect(parseHevcSpsResolution(buildSps({ width: 640, height: 480 })))
      .toEqual({ width: 640, height: 480 });
    expect(parseHevcSpsResolution(buildSps({ width: 1280, height: 720 })))
      .toEqual({ width: 1280, height: 720 });
    expect(parseHevcSpsResolution(buildSps({ width: 1920, height: 1080 })))
      .toEqual({ width: 1920, height: 1080 });
  });

  it('crops the conformance window in chroma units (4:2:0)', () => {
    // Encoders pad 1080p to 1088 luma rows; bottom offset 4 chroma = 8 luma
    expect(
      parseHevcSpsResolution(
        buildSps({ width: 1920, height: 1088, crop: { bottom: 4 } })
      )
    ).toEqual({ width: 1920, height: 1080 });
    expect(
      parseHevcSpsResolution(
        buildSps({ width: 640, height: 368, crop: { bottom: 4 } })
      )
    ).toEqual({ width: 640, height: 360 });
  });

  it('crops 4:4:4 (chromaFormatIdc 3) offsets in luma samples', () => {
    expect(
      parseHevcSpsResolution(
        buildSps({
          width: 642,
          height: 482,
          chromaFormatIdc: 3,
          crop: { left: 1, right: 1, top: 1, bottom: 1 },
        })
      )
    ).toEqual({ width: 640, height: 480 });
  });

  it('treats monochrome (chromaFormatIdc 0) offsets as luma samples', () => {
    expect(
      parseHevcSpsResolution(
        buildSps({
          width: 642,
          height: 480,
          chromaFormatIdc: 0,
          crop: { right: 2 },
        })
      )
    ).toEqual({ width: 640, height: 480 });
  });

  it('skips sub-layer profile_tier_level fields', () => {
    expect(
      parseHevcSpsResolution(
        buildSps({ width: 1280, height: 720, maxSubLayersMinus1: 2 })
      )
    ).toEqual({ width: 1280, height: 720 });
  });

  it('strips emulation-prevention bytes before parsing', () => {
    const withEp = buildSps({
      width: 1920,
      height: 1080,
      emulationPrevention: true,
    });
    // The compat flags 0x60000000 guarantee 00 00 sequences in the RBSP,
    // so EP insertion must actually have happened for this case.
    let found = false;
    for (let i = 0; i + 2 < withEp.length; i++) {
      if (withEp[i] === 0 && withEp[i + 1] === 0 && withEp[i + 2] === 3) {
        found = true;
        break;
      }
    }
    expect(found).toBe(true);
    expect(parseHevcSpsResolution(withEp)).toEqual({
      width: 1920,
      height: 1080,
    });
  });

  it('returns null for malformed input', () => {
    expect(parseHevcSpsResolution(new Uint8Array([0x42, 0x01, 0x01]))).toBeNull();
    expect(parseHevcSpsResolution(new Uint8Array(0))).toBeNull();
  });
});

describe('createHevcInitSegment dimensions', () => {
  const vps = new Uint8Array([0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01]);
  const pps = new Uint8Array([0x68, 0x01, 0xab]);

  /** Minimal ISOBMFF box walker: returns the content of the box at `path`. */
  function findBoxContent(buf: Uint8Array, path: string[]): Uint8Array | null {
    // stsd is a full box: 4 bytes version/flags + 4 bytes entry count
    // precede the sample entries
    const childPayloadStart: Record<string, number> = { stsd: 8 };
    let current: {
      type: string;
      content: Uint8Array;
      childrenStart: number;
    }[] = [{ type: '', content: buf, childrenStart: 0 }];
    for (const name of path) {
      const next: {
        type: string;
        content: Uint8Array;
        childrenStart: number;
      }[] = [];
      for (const parent of current) {
        let off = parent.childrenStart;
        while (off + 8 <= parent.content.length) {
          const size =            (parent.content[off] << 24)
            | (parent.content[off + 1] << 16)
            | (parent.content[off + 2] << 8)
            | parent.content[off + 3];
          if (size < 8 || off + size > parent.content.length) break;
          const type = String.fromCharCode(
            parent.content[off + 4],
            parent.content[off + 5],
            parent.content[off + 6],
            parent.content[off + 7]
          );
          next.push({
            type,
            content: parent.content.subarray(off + 8, off + size),
            childrenStart: childPayloadStart[type] ?? 0,
          });
          off += size;
        }
      }
      const matches = next.filter(b => b.type === name);
      if (matches.length === 0) return null;
      current = matches;
    }
    return current[0].content;
  }

  function readU16(buf: Uint8Array, offset: number): number {
    return (buf[offset] << 8) | buf[offset + 1];
  }

  beforeAll(() => {
    // pickMimeCodec probes MediaSource.isTypeSupported before building the
    // segment; jsdom has no MediaSource so stub a positive answer.
    vi.stubGlobal('MediaSource', {
      isTypeSupported: () => true,
    });
  });

  it('declares the real stream size in hvc1/tkhd, not a hardcoded 1080p', () => {
    const built = createHevcInitSegment(
      vps,
      buildSps({ width: 640, height: 480 }),
      pps
    );
    expect(built).not.toBeNull();
    const { segment } = (built!);

    // hvc1 sample entry: width/height u16 at content offsets 24/26
    const hvc1 = findBoxContent(segment, ['moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'hvc1']);
    expect(hvc1).not.toBeNull();
    expect(readU16(hvc1!, 24)).toBe(640);
    expect(readU16(hvc1!, 26)).toBe(480);

    // tkhd: width/height stored as 16.16 fixed point
    const tkhd = findBoxContent(segment, ['moov', 'trak', 'tkhd']);
    expect(tkhd).not.toBeNull();
    // tkhd: width/height stored as 16.16 fixed point; the returned content
    // starts with the 4-byte version/flags of the full box, so add 4
    expect(readU16(tkhd!, 76)).toBe(640);
    expect(readU16(tkhd!, 78)).toBe(0);
    expect(readU16(tkhd!, 80)).toBe(480);
    expect(readU16(tkhd!, 82)).toBe(0);
  });

  it('propagates a cropped visible size into the container', () => {
    const built = createHevcInitSegment(
      vps,
      buildSps({ width: 1280, height: 736, crop: { bottom: 8 } }),
      pps
    );
    expect(built).not.toBeNull();
    const hvc1 = findBoxContent(built!.segment, [
      'moov', 'trak', 'mdia', 'minf', 'stbl', 'stsd', 'hvc1',
    ]);
    expect(readU16(hvc1!, 24)).toBe(1280);
    expect(readU16(hvc1!, 26)).toBe(720);
  });

  it('returns null when the SPS cannot be parsed', () => {
    const built = createHevcInitSegment(vps, new Uint8Array([0x42, 0x01]), pps);
    expect(built).toBeNull();
  });
});
