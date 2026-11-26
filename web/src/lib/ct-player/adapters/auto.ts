import type { EncodedFrame, SourceAdapter } from '../shared/types';
import { nalCodecHint, scanAnnexBNalus } from '../media/nalu';
import { avccToAnnexB, concatBytes } from './base';
import { createAnnexbAdapter } from './annexb';
import { createAvccAdapter } from './avcc';

/** Automatic detection is limited to formats with verifiable signatures. */
export interface AutoAdapterOptions {
  maxBytes?: number;
  maxMessages?: number;
}

export function createAutoAdapter(options: AutoAdapterOptions = {}): SourceAdapter {
  const maxBytes = bounded(options.maxBytes, 8 * 1024 * 1024, 1024, 32 * 1024 * 1024);
  const maxMessages = bounded(options.maxMessages, 120, 2, 1000);
  let messages: { data: Uint8Array; timestamp?: { pts90k?: number; dts90k?: number } }[] = [];
  let size = 0;
  let selected: SourceAdapter | null = null;
  let failed = false;
  return {
    name: 'auto',
    isDetecting: () => !selected && !failed,
    push(chunk: Uint8Array, timestamp?: { pts90k?: number; dts90k?: number }): EncodedFrame[] {
      if (failed) return [];
      if (selected) return selected.push(chunk, timestamp);
      if (size + chunk.length > maxBytes || messages.length >= maxMessages) {
        failed = true;
        messages = [];
        throw new Error('Automatic stream format detection failed: limit reached; select the adapter manually');
      }
      messages.push({ data: chunk.slice(), timestamp });
      size += chunk.length;
      const kind = detect(messages.map(message => message.data));
      if (!kind) return [];
      if (kind !== 'avcc' && messages.some(message => message.timestamp !== undefined)) {
        failed = true;
        messages = [];
        throw new Error('Header PTS requires one AVCC access unit per message');
      }
      selected = kind === 'avcc' ? createAvccAdapter() : createAnnexbAdapter();
      const buffered = messages;
      messages = [];
      size = 0;
      return buffered.flatMap(message => selected!.push(message.data, message.timestamp));
    },
    reset() {
      selected?.reset();
      selected = null;
      messages = [];
      size = 0;
      failed = false;
    },
  };
}

function bounded(value: number | undefined, fallback: number, min: number, max: number): number {
  return typeof value === 'number' && Number.isInteger(value) && value >= min && value <= max ? value : fallback;
}

function hasParameterSet(data: Uint8Array): boolean {
  return scanAnnexBNalus(data).some(n => n.payloadLen > 0 && nalCodecHint(n.rawHeader) !== null);
}

function detect(messages: Uint8Array[]): 'avcc' | 'annexb' | null {
  // A subscription can start between keyframes, so the first complete AVCC
  // message may not contain parameter sets. Check later messages as well.
  for (const message of messages) {
    const avcc = avccToAnnexB(message);
    if (avcc && hasParameterSet(avcc)) return 'avcc';
  }
  // Do not scan arbitrary offsets: private headers followed by Annex B are ambiguous.
  let prefix: Uint8Array = new Uint8Array(0);
  for (const message of messages) prefix = concatBytes(prefix, message);
  const start = (prefix[0] === 0 && prefix[1] === 0 && (prefix[2] === 1 || (prefix[2] === 0 && prefix[3] === 1)));
  if (start && hasParameterSet(prefix)) return 'annexb';
  return null;
}
