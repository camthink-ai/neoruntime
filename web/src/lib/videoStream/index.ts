/**
 * Video Stream Library
 * Unified video player interface supporting multiple streaming protocols
 */

// 协议无关播放器封装 — delegates to the vendored CT player engine
// (WebCodecs → MSE → WASM software decode fallback chain).
export { VideoStreamPlayer, deviceAdapter } from './player';
export type {
  StreamPlayerOptions,
  StreamPlayerStats,
  StreamProtocol,
} from './types';
