/**
 * Video Stream Library
 * Unified video player interface supporting multiple streaming protocols
 */

// Protocol-agnostic player facade — delegates to the vendored CT player engine
// (WebCodecs → MSE → WASM software decode fallback chain).
export { VideoStreamPlayer, deviceAdapter } from './player';
export type {
  StreamPlayerOptions,
  StreamPlayerStats,
  StreamProtocol,
} from './types';
