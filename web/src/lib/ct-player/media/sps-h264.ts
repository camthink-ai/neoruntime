// @ts-nocheck
/**
 * H.264 SPS 解析（Exp-Golomb）：分辨率与 codec string。
 * 移植自底座项目并保持容错语义：解析失败时由调用者决定回退默认值。
 */

class BitReader {
  private bitPos = 0;

  constructor(private data: Uint8Array) {}

  readBit(): number {
    const byteIdx = this.bitPos >> 3;
    const bitIdx = 7 - (this.bitPos & 7);
    this.bitPos++;
    if (byteIdx >= this.data.length) return 0;
    return (this.data[byteIdx] >> bitIdx) & 1;
  }

  readBits(n: number): number {
    let v = 0;
    for (let i = 0; i < n; i++) v = (v << 1) | this.readBit();
    return v;
  }

  readUE(): number {
    let zeros = 0;
    while (this.readBit() === 0 && zeros < 32) zeros++;
    return zeros > 0 ? (1 << zeros) - 1 + this.readBits(zeros) : 0;
  }
}

/** 宏块对齐的编码尺寸（未裁剪）。 */
export function parseH264SpsCodedSize(sps: Uint8Array): { width: number; height: number } | null {
  try {
    if (sps.length < 5) return null;
    const br = new BitReader(sps);
    br.readBits(8 * 4); // NAL header + profile + compat + level
    const profileIdc = sps[1];
    br.readUE(); // seq_parameter_set_id
    if ([100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134].includes(profileIdc)) {
      const chromaFormatIdc = br.readUE();
      if (chromaFormatIdc === 3) br.readBit(); // separate_colour_plane_flag
      br.readUE(); // bit_depth_luma_minus8
      br.readUE(); // bit_depth_chroma_minus8
      br.readBit(); // qpprime_y_zero_transform_bypass_flag
      if (br.readBit()) {
        // seq_scaling_matrix_present_flag
        const limit = chromaFormatIdc !== 3 ? 8 : 12;
        for (let i = 0; i < limit; i++) {
          if (br.readBit()) {
            const size = i < 6 ? 16 : 64;
            let lastScale = 8;
            let nextScale = 8;
            for (let j = 0; j < size; j++) {
              if (nextScale !== 0) {
                const code = br.readUE();
                const delta = code & 1 ? (code + 1) >> 1 : -(code >> 1);
                nextScale = (lastScale + delta + 256) % 256;
              }
              lastScale = nextScale === 0 ? lastScale : nextScale;
            }
          }
        }
      }
    }
    br.readUE(); // log2_max_frame_num_minus4
    const picOrderCntType = br.readUE();
    if (picOrderCntType === 0) {
      br.readUE(); // log2_max_pic_order_cnt_lsb_minus4
    } else if (picOrderCntType === 1) {
      br.readBit(); // delta_pic_order_always_zero_flag
      br.readUE(); // offset_for_non_ref_pic
      br.readUE(); // offset_for_top_to_bottom_field
      const cnt = br.readUE();
      for (let i = 0; i < cnt; i++) br.readUE();
    }
    br.readUE(); // max_num_ref_frames
    br.readBit(); // gaps_in_frame_num_value_allowed_flag
    const picWidthInMbsMinus1 = br.readUE();
    const picHeightInMapUnitsMinus1 = br.readUE();
    const frameMbsOnlyFlag = br.readBit();
    const width = (picWidthInMbsMinus1 + 1) * 16;
    const height = (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16;
    if (width <= 0 || height <= 0) return null;
    return { width, height };
  } catch {
    return null;
  }
}

/** 从 SPS 生成 avc1 codec string（含 profile/compat/level）。 */
export function h264CodecString(sps: Uint8Array): string {
  if (sps.length < 4) return 'avc1.42E01E';
  const hex = (v: number) => v.toString(16).padStart(2, '0');
  return `avc1.${hex(sps[1])}${hex(sps[2])}${hex(sps[3])}`;
}
