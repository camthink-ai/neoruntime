import type { LatencyPresetName } from '../shared/types';
import type { PlatformInfo } from '../shared/capability';

/**
 * Latency presets. The balanced profile favors smooth playback across jittery Wi-Fi links.
 */
export interface LatencyParams {
  /** Startup buffer in milliseconds. */
  startBufferMs: number;
  /** Presentation jitter buffer in milliseconds for WebCodecs and timestamped WASM frames. */
  jitterBufferMs: number;
  /** Delay in milliseconds above which playback rate increases to catch up. */
  chaseStartMs: number;
  /** Catch-up playback rate. */
  chaseRate: number;
  /** Delay in milliseconds above which playback seeks to the live edge. */
  hardSeekMs: number;
  /** Cooldown between hard seeks, in milliseconds. */
  seekCooldownMs: number;
  /** Buffer retained behind the live edge, in seconds. */
  bufferWindowSec: number;
  /** Maximum pending frame count. */
  maxPendingFrames: number;
  /** Whether playback-rate catch-up is supported; Safari uses seeks instead. */
  usePlaybackRate: boolean;
}

const PRESET_VALUES: Record<LatencyPresetName, Omit<LatencyParams, 'usePlaybackRate'>> = {
  // No startup or jitter buffer. Network jitter directly affects playback.
  raw: {
    startBufferMs: 0,
    jitterBufferMs: 0,
    chaseStartMs: 300,
    chaseRate: 1.1,
    hardSeekMs: 500,
    seekCooldownMs: 2000,
    bufferWindowSec: 1,
    maxPendingFrames: 10,
  },
  ultra: {
    startBufferMs: 300,
    jitterBufferMs: 50,
    chaseStartMs: 800,
    chaseRate: 1.05,
    hardSeekMs: 1000,
    seekCooldownMs: 5000,
    bufferWindowSec: 2,
    maxPendingFrames: 20,
  },
  balanced: {
    startBufferMs: 600,
    jitterBufferMs: 100,
    chaseStartMs: 1500,
    chaseRate: 1.05,
    hardSeekMs: 3000,
    seekCooldownMs: 5000,
    bufferWindowSec: 3,
    maxPendingFrames: 30,
  },
  stable: {
    startBufferMs: 1000,
    jitterBufferMs: 200,
    chaseStartMs: 2500,
    chaseRate: 1.1,
    hardSeekMs: 5000,
    seekCooldownMs: 5000,
    bufferWindowSec: 5,
    maxPendingFrames: 60,
  },
};

export function resolveLatencyParams(
  preset: LatencyPresetName,
  platform?: PlatformInfo
): LatencyParams {
  const values = PRESET_VALUES[preset];
  const chaseByRate = !(platform?.iosWebkit || platform?.safari);
  return { ...values, usePlaybackRate: chaseByRate };
}
