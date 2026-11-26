import type { AdapterDescriptor, SourceAdapter } from '../shared/types';
import { createAutoAdapter } from './auto';
import { createAnnexbAdapter } from './annexb';
import { createAvccAdapter } from './avcc';

export type AdapterFactory = (options?: Record<string, unknown>) => SourceAdapter;

/** Built-in adapter registry shared by the worker and main-thread fallback. */
export const BUILTIN_ADAPTERS: Record<string, AdapterFactory> = {
  auto: (options) => createAutoAdapter(options),
  annexb: (options) => createAnnexbAdapter((options ?? {}) as never),
  avcc: (options) => createAvccAdapter((options ?? {}) as { maxBufferBytes?: number }),
};

export function createBuiltinAdapter(desc: AdapterDescriptor): SourceAdapter | null {
  const factory = BUILTIN_ADAPTERS[desc.name];
  if (!factory) return null;
  const adapter = factory(desc.options);
  const headerBytes = desc.options?.headerBytes ?? 0;
  if (typeof headerBytes !== 'number' || !Number.isSafeInteger(headerBytes) || headerBytes < 0 || headerBytes > 65536) {
    throw new Error('headerBytes must be an integer between 0 and 65536');
  }
  const ptsOffset = desc.options?.ptsOffset;
  if (ptsOffset !== undefined && ((desc.name !== 'avcc' && desc.name !== 'auto') || typeof ptsOffset !== 'number' || !Number.isSafeInteger(ptsOffset) || ptsOffset < 0 || ptsOffset + 4 > headerBytes)) {
    throw new Error('ptsOffset requires AVCC or auto and a 4-byte timestamp within the message header');
  }
  if (headerBytes === 0) return adapter;
  return {
    name: adapter.name,
    ...(adapter.isDetecting ? { isDetecting: () => adapter.isDetecting!() } : {}),
    push(chunk) {
      if (chunk.length <= headerBytes) return [];
      const timestamp = typeof ptsOffset === 'number' ? { pts90k: new DataView(chunk.buffer, chunk.byteOffset + ptsOffset, 4).getUint32(0, false) } : undefined;
      return adapter.push(chunk.subarray(headerBytes), timestamp);
    },
    reset() { adapter.reset(); },
  };
}

export function isBuiltinAdapter(name: string): boolean {
  return name in BUILTIN_ADAPTERS;
}

