import type { EncodedFrame, SourceAdapter } from '../shared/types';
import { NaluFramer, normalizeToAnnexB } from './base';

/** AVCC（4 字节大端长度前缀）适配器：每条消息为自包含 NALU 序列。 */
export function createAvccAdapter(): SourceAdapter {
  const framer = new NaluFramer({ framing: 'message' });
  return {
    name: 'avcc',
    push(chunk: Uint8Array, timestamp?: { pts90k?: number; dts90k?: number }): EncodedFrame[] {
      const annexb = normalizeToAnnexB(chunk);
      const frames = framer.push(annexb, timestamp);
      return [...frames, ...framer.endMessage(timestamp)];
    },
    reset() {
      framer.reset();
    },
  };
}
