/** 测试夹具：构造带真实头字节的 AnnexB NALU。 */

export const SC4 = [0, 0, 0, 1];

/** h264 NAL 头：forbidden(1)=0, nal_ref_idc(2), type(5) */
const H264_HEADERS: Record<number, number> = {
  1: 0x41, // non-IDR slice (ref_idc=2)
  5: 0x65, // IDR (ref_idc=3)
  6: 0x06, // SEI (ref_idc=0)
  7: 0x67, // SPS (ref_idc=3)
  8: 0x68, // PPS (ref_idc=3)
  9: 0x09, // AUD (ref_idc=0)
};

export function h264Nal(type: number, payload: number[] = [0x11, 0x22, 0x33]): Uint8Array {
  const header = H264_HEADERS[type] ?? (0x60 | type);
  return new Uint8Array([...SC4, header, ...payload]);
}

/** h265 NAL 头：forbidden(1)=0, type(6), layer(6)+tid(3)=0x01 */
export function h265Nal(type: number, payload: number[] = [0x11, 0x22]): Uint8Array {
  return new Uint8Array([...SC4, type << 1, 0x01, ...payload]);
}

export function concatAll(...parts: Uint8Array[]): Uint8Array {
  const total = parts.reduce((s, p) => s + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out;
}
