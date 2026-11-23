/**
 * Main entry point for the headless player engine and its events.
 * Vanilla: createPlayer(opts).attach('#el').then(play({ source }))
 * React/Vue: see the '@camthink/player/react' and '@camthink/player/vue' entry points.
 */

export { VERSION } from './version';
export { CTPlayer, createPlayer } from './core/player';
export { detectCapabilities, detectPlatform, resetCapabilityCache } from './shared/capability';
export type { CapabilityProfile, PlatformInfo } from './shared/capability';
export { BUILTIN_ADAPTERS, createBuiltinAdapter, isBuiltinAdapter } from './adapters/registry';
export type { AdapterFactory } from './adapters/registry';
export { createAutoAdapter } from './adapters/auto';
export type { AutoAdapterOptions } from './adapters/auto';
export { NaluFramer } from './adapters/base';
export { PtsReorderBuffer } from './media/reorder';
export type { PtsReorderOptions, ReleasedEntry } from './media/reorder';
export { PacedPresenter } from './media/paced-presenter';
export type { PacedPresenterOptions } from './media/paced-presenter';
export { resolveLatencyParams } from './core/presets';
export type { LatencyParams } from './core/presets';
export { EventBus } from './shared/events';
export type {
  CTEventMap,
  FallbackEvent,
  FallbackLevel,
  StateEvent,
  Unsubscribe,
} from './shared/events';
export type { CTError, CTErrorCode } from './shared/errors';
export type * from './shared/types';
export { DEFAULT_RECONNECT_POLICY } from './transport/reconnect';
export { PcmRing } from './audio/pcm-ring';
export { PcmAudioChannel } from './audio/pcm-channel';
export { AudioTalker } from './audio/talker';

