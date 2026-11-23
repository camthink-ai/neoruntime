import { describe, expect, it } from 'vitest';
import { deviceAdapter } from './player';

const idr = [0, 0, 0, 3, 0x40, 1, 0xaa, 0, 0, 0, 4, 0x26, 1, 0x80, 0x55];
const delta = [0, 0, 0, 4, 0x02, 1, 0x80, 0x55];
function packet(header: 10 | 18, pts: number, payload: number[]) {
  const bytes = new Uint8Array(header + payload.length);
  const view = new DataView(bytes.buffer);
  view.setUint32(0, 0xffd8ff00, false);
  bytes[5] = header === 18 ? 1 : 0;
  view.setUint32(6, pts, false);
  bytes.set(payload, header);
  return bytes;
}

describe('device video adapter', () => {
  it('preserves decode order and the 18-byte header PTS for H.265', () => {
    const adapter = deviceAdapter();
    const frames = [
      ...adapter.push(packet(18, 18000, idr)),
      ...adapter.push(packet(18, 9000, delta)),
      ...adapter.push(packet(18, 12000, delta)),
    ];
    expect(frames.map(frame => frame.pts90k)).toEqual([18000, 9000, 12000]);
    expect(frames.map(frame => frame.seq)).toEqual([0, 1, 2]);
    expect(frames[0].nalus.map(nal => nal.type)).toEqual([32, 19]);
  });

  it('supports the older 10-byte header and headerless messages after reset', () => {
    const adapter = deviceAdapter();
    expect(adapter.push(packet(10, 9000, idr))[0].pts90k).toBe(9000);
    adapter.reset();
    expect(adapter.push(new Uint8Array(idr))[0].pts90k).toBeUndefined();
  });
});
