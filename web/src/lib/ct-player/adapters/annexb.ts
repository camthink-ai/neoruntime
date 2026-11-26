import type { EncodedFrame, SourceAdapter } from '../shared/types';
import { NaluFramer } from './base';

export interface AnnexBAdapterOptions {
  /** 'stream'（默认）：字节流可任意切断；'message'：每条消息自包含 AU */
  framing?: 'stream' | 'message';
}

/** 通用 AnnexB 裸流适配器（无帧头协议）。 */
export function createAnnexbAdapter(options: AnnexBAdapterOptions = {}): SourceAdapter {
  const framer = new NaluFramer({ framing: options.framing ?? 'stream' });
  return {
    name: 'annexb',
    push(chunk: Uint8Array): EncodedFrame[] {
      const frames = framer.push(chunk);
      if (options.framing === 'message') {
        return [...frames, ...framer.endMessage()];
      }
      return frames;
    },
    reset() {
      framer.reset();
    },
  };
}
