export type StreamProtocol = 'mse-h264' | 'webrtc' | 'hls';

export interface StreamPlayerStats {
  bandwidth: number;
  latency: number;
  bufferTime: number;
  playbackRate: number;
  packetsPerSecond: number;
}

export interface StreamPlayerOptions {
  /**
   * 播放协议类型，当前仅实现 mse-h264，预留 webrtc / hls
   */
  protocol?: StreamProtocol;

  /**
   * 内部事件回调（目前与 msePlayer 保持一致）
   */
  onEvent?: (event: { t: string }) => void;

  /**
   * Snapshot width from configured encoder resolution.
   * H.264 encoder pads frame dimensions to 16px macroblock boundaries
   * (e.g. 1080→1088, 360→368). Pass the actual configured width so
   * snapshots use the correct dimensions instead of padded ones.
   */
  snapshotWidth?: number;

  /**
   * Snapshot height from configured encoder resolution.
   * H.264 encoder pads frame dimensions to 16px macroblock boundaries
   * (e.g. 1080→1088, 360→368). Pass the actual configured height so
   * snapshots use the correct dimensions instead of padded ones.
   */
  snapshotHeight?: number;
}
