/**
 * Video Stream Library
 * Unified video player interface supporting multiple streaming protocols
 */

// Legacy exports (for backward compatibility)
export { default as H264Player } from './MSE/msePlayer';

// 新的协议无关播放器封装
export { VideoStreamPlayer } from './player';
export type {
  StreamPlayerOptions,
  StreamPlayerStats,
  StreamProtocol,
} from './types';
