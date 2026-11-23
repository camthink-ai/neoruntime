import { describe, expect, it } from 'vitest';
import { deviceAdapter } from './player';

const idr = [0, 0, 0, 3, 0x40, 1, 0xaa, 0, 0, 0, 4, 0x26, 1, 0x80, 0x55];
const delta = [0, 0, 0, 4, 0x02, 1, 0x80, 0x55];
function packet(pts: number, payload: number[]) {
  const bytes = new Uint8Array(18 + payload.length);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0xffd8ff00, false);
  bytes[5] = 1;
  view.setUint32(6, pts, false);
  bytes.set(payload, 18);
  return bytes;
}

describe('device video adapter', () => {
  it('strips the fixed 18-byte header and preserves the PTS at offset 6', () => {
    const adapter = deviceAdapter();
    const frames = [
      ...adapter.push(packet(18000, idr)),
      ...adapter.push(packet(9000, delta)),
      ...adapter.push(packet(12000, delta)),
    ];
    expect(frames.map(frame => frame.pts90k)).toEqual([18000, 9000, 12000]);
    expect(frames.map(frame => frame.seq)).toEqual([0, 1, 2]);
    expect(frames[0].nalus.map(nal => nal.type)).toEqual([32, 19]);
  });

  it('ignores empty messages', () => {
    const adapter = deviceAdapter();
    expect(adapter.push(new Uint8Array(0))).toEqual([]);
  });
});
