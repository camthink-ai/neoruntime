export type StreamProtocol = 'mse-h264' | 'webrtc' | 'hls';

export interface StreamPlayerStats {
  bandwidth: number;
  latency: number;
  bufferTime: number;
  playbackRate: number;
  packetsPerSecond: number;
}

export interface StreamPlayerOptions {
  objectFit?: 'contain' | 'cover';
  /**
   * Playback protocol type; only mse-h264 is implemented, webrtc / hls reserved.
   */
  protocol?: StreamProtocol;

  /**
   * Internal event callbacks (kept from the legacy player contract).
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
