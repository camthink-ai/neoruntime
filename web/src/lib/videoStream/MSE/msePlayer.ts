import MsMediaSource from './media.js';
import { MP4Muxer } from './utils/h264ToMp4.ts';
import {
  createHevcInitSegment,
  HevcMp4Muxer,
  parseHevcSpsResolution,
} from './utils/hevcMp4.ts';
import WebCodecsPlayer, {
  isWebCodecsSupported,
} from '../WebCodecs/webcodecsPlayer.js';

// NALU Type Constants (H.264)
const NAL_TYPE_SPS = 7;
const NAL_TYPE_PPS = 8;
const NAL_TYPE_IDR = 5; // Used for fallback keyframe detection

// HEVC (H.265) NAL types (nuh: first byte >> 1 & 0x3f, two-byte NAL header)
const HEVC_NAL_VPS = 32;
const HEVC_NAL_SPS = 33;
const HEVC_NAL_PPS = 34;

/**
 * @TODO
 * Player log panel
 * - WebSocket packet transmission rate
 * - Video stream frame rate
 * - Packet loss rate
 * - Latency
 * - Buffer data amount
 * -
 */
// Type definitions
interface CallbackEvent {
  t: string;
  codec?: string; // Used by 'codecMismatch' event to report unsupported codec
}

type CallbackFunction = (event: CallbackEvent) => void;

interface ReturnResult {
  e: number;
  m: string;
}

/**
 * @description: H264 player class
 */
export default class H264Player {
  private videoPlayer: MsMediaSource | WebCodecsPlayer | null = null;

  private cb: CallbackFunction;

  private useWebCodecs: boolean = false; // Track which decoder is in use

  private useMSEFallback: boolean = false; // Track if we've fallen back to MSE

  private type: string = 'video';

  private videoElement: HTMLVideoElement | null = null;

  // private hederBits: Uint8Array = new Uint8Array(0);

  public packetCount: number = 0;

  public packetsPerSecond: number = 0;

  private lastPacketSec: number = 0;

  public currentTime: number = 0;

  public currentFrame: number = 0;

  public totalBytesLoaded: number = 0;

  // Latency tracking
  public latency: number = 0;

  // Bandwidth tracking
  private lastBandwidthCheck: number = 0;

  private lastBytesLoaded: number = 0;

  public bandwidth: number = 0; // kBps

  // Track initialization state
  private isInitialized: boolean = false;

  private mp4Muxer: MP4Muxer | null = null;

  private hasReceivedFirstIFrame: boolean = false;

  private ppsNalu: Uint8Array | null = null;

  private spsNalu: Uint8Array | null = null;

  private spsWidth: number = 1920;

  private spsHeight: number = 1080;

  /** Annex-B sniff: 0x67/0x68 → H.264, VPS/SPS/PPS 特征 → H.265 */
  private videoCodec: 'h264' | 'h265' | 'unknown' = 'unknown';

  private vpsNalu: Uint8Array | null = null;

  private hevcMuxer: HevcMp4Muxer | null = null;

  // Frame rate detection
  private lastPts90kHz: number | undefined = undefined; // Previous frame PTS for FPS detection

  private detectedFrameRate: number = 30;

  // Next timestamp for continuous MSE timeline (90kHz)
  private nextTimestamp: number = 0;

  // First PTS (90kHz) seen since last (re)init — used to normalize PTS for CTO calc.
  // Undefined means "not yet seen any frame since init".
  private firstPts90kHz: number | undefined = undefined;

  private statsIntervalId: ReturnType<typeof setInterval> | null = null;

  private boundVisibilityHandler: (() => void) | null = null;

  // Add getter method for debugging
  getCurrentTime(): number {
    return this.currentTime;
  }

  constructor(cb: CallbackFunction) {
    this.cb = cb;
    this.createVideoPlayer();
    this.packetCount = 0;
    this.packetsPerSecond = 0;
    this.lastPacketSec = 0;
    this.currentTime = 0;
    this.currentFrame = 0;
    this.totalBytesLoaded = 0;
    this.hasReceivedFirstIFrame = false;
    this.detectedFrameRate = 30;
  }

  /**
   * Obsolete: Maintained for compatibility with VideoStreamPlayer
   */
  public initDecoder(): this {
    return this;
  }

  /**
   * Obsolete: Maintained for compatibility with VideoStreamPlayer
   */
  public initDecoderForNewStream(): void {
    this.isInitialized = false;
    this.spsNalu = null;
    this.ppsNalu = null;
    this.mp4Muxer = null;
    this.hasReceivedFirstIFrame = false;
    this.detectedFrameRate = 30;
    this.lastPts90kHz = undefined;
    this.spsWidth = 0;
    this.spsHeight = 0;
    this.firstPts90kHz = undefined;
    this.nextTimestamp = 0;
    this.videoCodec = 'unknown';
    this.vpsNalu = null;
    this.hevcMuxer = null;

    if (this.videoPlayer) {
      // Properly tear down old MediaSource/SourceBuffer before resetting —
      // resetInitFlag alone leaves orphaned MSE objects that corrupt the decoder.
      if (this.videoPlayer instanceof MsMediaSource) {
        this.videoPlayer.cleanupForNewStream();
      } else {
        // For WebCodecs, just uninitialize
        this.videoPlayer.uninitMse();
      }
    }
  }

  /**
   * 处理来自 WebSocket 的视频数据
   * - 统计包速 / 带宽
   * - 解析 NALU 单元
   * - 交给 MP4Muxer 打包为 fMP4 并输出给 MSE
   */
  public handleVideoData(payload: ArrayBuffer): void {
    // Count packets per second
    const nowSec = Math.floor(Date.now() / 1000);
    if (this.lastPacketSec !== nowSec) {
      if (this.lastPacketSec > 0) {
        this.packetsPerSecond = this.packetCount;
      }
      this.lastPacketSec = nowSec;
      this.packetCount = 0;
    }
    this.packetCount++;

    // Track total bytes for bandwidth calculation
    this.totalBytesLoaded += payload.byteLength;

    // Parse frame header (V1: 10 bytes, V2: 18 bytes with DTS)
    let h264Data: Uint8Array;
    let isKeyframe: boolean = false;
    let pts90kHz: number = 0; // Presentation Time Stamp
    let dts90kHz: number = 0; // Decode Time Stamp

    if (payload.byteLength >= 10) {
      const dataView = new DataView(payload);
      const magic = dataView.getUint32(0, false); // Big-endian

      if (magic === 0xffd8ff00) {
        // Protocol with header
        const frameType = dataView.getUint8(4);
        isKeyframe = frameType === 0;

        // Extract flags (byte 5) to detect V2 protocol
        const flags = dataView.getUint8(5);
        const hasDts = (flags & 0x01) !== 0;

        // Extract PTS (bytes 6-9)
        pts90kHz = dataView.getUint32(6, false); // Big-endian

        if (payload.byteLength >= 18 && hasDts) {
          // V2 protocol: has DTS (bytes 10-13)
          dts90kHz = dataView.getUint32(10, false); // Big-endian
          // Skip 18-byte header
          h264Data = new Uint8Array(payload, 18);
        } else {
          // V1 protocol: no DTS
          dts90kHz = pts90kHz; // For V1, DTS = PTS
          // Skip 10-byte header
          h264Data = new Uint8Array(payload, 10);
        }
      } else {
        // Old protocol: no header (fallback for compatibility)
        h264Data = new Uint8Array(payload);
        isKeyframe = this.detectKeyframe(h264Data);
      }
    } else {
      h264Data = new Uint8Array(payload);
    }

    // Convert to ArrayBuffer (create a copy to ensure type compatibility)
    const h264Copy = new Uint8Array(h264Data);
    this.dealVideoData(
      h264Copy.buffer.slice(0),
      isKeyframe,
      pts90kHz,
      dts90kHz
    );
  }

  /**
   * Detect if frame is keyframe by checking for IDR NAL unit
   */
  // eslint-disable-next-line class-methods-use-this
  private detectKeyframe(data: Uint8Array): boolean {
    // Search for IDR NAL type (type 5) in Annex-B format
    for (let i = 0; i < data.length - 4; i++) {
      if (
        data[i] === 0
        && data[i + 1] === 0
        && data[i + 2] === 0
        && data[i + 3] === 1
      ) {
        const b0 = data[i + 4];
        const nalType = b0 & 0x1f;
        if (nalType === NAL_TYPE_IDR) return true; // NAL_TYPE_IDR
        const hevcT = (b0 >> 1) & 0x3f;
        if (hevcT === 19 || hevcT === 20) return true;
      }
    }
    // Search in AVCC format (4-byte length prefix)
    for (let i = 0; i < data.length - 8; i++) {
      const length =        (data[i] << 24)
        | (data[i + 1] << 16)
        | (data[i + 2] << 8)
        | data[i + 3];
      if (length > 0 && length < data.length - i - 4) {
        const b0 = data[i + 4];
        const nalType = b0 & 0x1f;
        if (nalType === NAL_TYPE_IDR) return true;
        const hevcT = (b0 >> 1) & 0x3f;
        if (hevcT === 19 || hevcT === 20) return true;
      }
    }
    return false;
  }

  initPlayer(videoElement: HTMLVideoElement): this {
    this.videoElement = videoElement;
    this.videoPlayer = new MsMediaSource((msg: CallbackEvent) => {
      this.mediaSrcCallback(msg);
    });
    this.videoPlayer.setVideoElement(this.videoElement);
    this.setupProgressHandler();

    this.videoElement.addEventListener(
      'loadedmetadata',
      () => {
        if (this.videoElement && this.videoElement.paused) {
          this.videoElement.play().catch(() => {});
        }
      },
      { once: true }
    );

    // When the tab returns from background, Chrome may have paused
    // video-only media for power saving. Seek to the live edge and resume.
    this.boundVisibilityHandler = () => {
      if (document.hidden || !this.videoElement || !this.videoPlayer) return;

      this.seekToLiveEdge();

      if (this.videoElement.paused) {
        this.videoElement.play().catch(() => {});
      }
    };
    document.addEventListener('visibilitychange', this.boundVisibilityHandler);

    return this;
  }

  destroy(): void {
    if (this.statsIntervalId) {
      clearInterval(this.statsIntervalId);
      this.statsIntervalId = null;
    }
    if (this.boundVisibilityHandler) {
      document.removeEventListener(
        'visibilitychange',
        this.boundVisibilityHandler
      );
      this.boundVisibilityHandler = null;
    }
    this.stopPlay();
    this.videoElement = null;
  }

  /**
   * Pause decoding / playback（不再负责 WebSocket）
   */
  pause(): void {
    this.videoPlayer?.clearBuffer();
    if (this.videoElement && !this.videoElement.paused) {
      this.videoElement.pause();
    }
  }

  videoMsgCallback(event: {
    data: { data: ArrayBuffer; codec: string };
  }): void {
    this.videoPlayer?.processMp4VideoData(event, 0); // 0 = default snapshotFlag
  }

  /**
   * Clear the MSE source buffer and local frame buffer
   */
  public clearBuffer(): void {
    this.videoPlayer?.clearBuffer();
  }

  mediaSrcCallback(msg: CallbackEvent): void {
    if (msg.t === 'mseError') {
      // If using WebCodecs and got an error, fall back to MSE
      if (this.useWebCodecs && !this.useMSEFallback) {
        // console.warn('[H264Player] WebCodecs error, falling back to MSE')
        this.useMSEFallback = true;
        this.useWebCodecs = false;

        // Cleanup WebCodecs player
        if (this.videoPlayer) {
          this.videoPlayer.uninitMse();
        }

        // Create MSE player
        this.videoPlayer = new MsMediaSource((event: CallbackEvent) => {
          this.mediaSrcCallback(event);
        });

        // Reset initialization state for MSE
        this.isInitialized = false;
        this.mp4Muxer = null;
        this.hevcMuxer = null;
        this.hasReceivedFirstIFrame = false;
        this.nextTimestamp = 0;
        this.firstPts90kHz = undefined;
        this.currentFrame = 0;

        // Set video element for MSE player
        if (this.videoElement) {
          this.videoPlayer.setVideoElement(this.videoElement);
        }
        return;
      }

      // MSE error handling (original logic)
      // Reset muxer and init state but KEEP spsNalu/ppsNalu —
      // the existing SPS/PPS are almost certainly still valid, so we can
      // create a new init segment immediately on the next frame instead of
      // waiting for a full SPS+PPS+IDR cycle (saves 1-2 seconds of black screen).
      this.isInitialized = false;
      this.mp4Muxer = null;
      this.hevcMuxer = null;
      this.hasReceivedFirstIFrame = false;
      this.nextTimestamp = 0; // Reset base timestamp for reconnect
      this.firstPts90kHz = undefined;
      this.currentFrame = 0;
    }
    this.cb(msg);
  }

  createVideoPlayer(): void {
    // Try WebCodecs first, fall back to MSE if not supported or on error
    if (isWebCodecsSupported() && !this.useMSEFallback) {
      // console.log('[H264Player] Using WebCodecs decoder')
      this.useWebCodecs = true;
      this.videoPlayer = new WebCodecsPlayer((msg: CallbackEvent) => {
        this.mediaSrcCallback(msg);
      });
    } else {
      // console.log('[H264Player] Using MSE decoder')
      this.useWebCodecs = false;
      this.videoPlayer = new MsMediaSource((msg: CallbackEvent) => {
        this.mediaSrcCallback(msg);
      });
    }
  }

  /**
   * Parse resolution from SPS using Exp-Golomb decoding
   */
  private static parseSPSResolution(sps: Uint8Array): {
    width: number;
    height: number;
  } {
    try {
      if (sps.length < 5) return { width: 1920, height: 1080 };
      let bitPos = 0;
      const totalBits = sps.length * 8;
      const readBit = (): number => {
        if (bitPos >= totalBits) return 0;
        const byteIdx = bitPos >> 3;
        const bitIdx = 7 - (bitPos & 7);
        bitPos++;
        return (sps[byteIdx] >> bitIdx) & 1;
      };
      const readBits = (n: number): number => {
        let val = 0;
        for (let i = 0; i < n; i++) val = (val << 1) | readBit();
        return val;
      };
      const readUE = (): number => {
        let zeros = 0;
        while (readBit() === 0 && zeros < 32) zeros++;
        return zeros > 0 ? (1 << zeros) - 1 + readBits(zeros) : 0;
      };
      bitPos = 8 * 4;
      const profileIdc = sps[1];
      readUE();
      if (
        [100, 110, 122, 244, 44, 83, 86, 118, 128, 138, 139, 134].includes(
          profileIdc
        )
      ) {
        const chromaFormatIdc = readUE();
        if (chromaFormatIdc === 3) readBit();
        readUE();
        readUE();
        readBit();
        if (readBit()) {
          const limit = chromaFormatIdc !== 3 ? 8 : 12;
          for (let i = 0; i < limit; i++) {
            if (readBit()) {
              const size = i < 6 ? 16 : 64;
              let lastScale = 8;
              let nextScale = 8;
              for (let j = 0; j < size; j++) {
                if (nextScale !== 0) {
                  const code = readUE();
                  const delta = code & 1 ? (code + 1) >> 1 : -(code >> 1);
                  nextScale = (lastScale + delta + 256) % 256;
                }
                lastScale = nextScale === 0 ? lastScale : nextScale;
              }
            }
          }
        }
      }
      readUE();
      const picOrderCntType = readUE();
      if (picOrderCntType === 0) {
        readUE();
      } else if (picOrderCntType === 1) {
        readBit();
        readUE();
        readUE();
        const cnt = readUE();
        for (let i = 0; i < cnt; i++) readUE();
      }
      readUE();
      readBit();
      const picWidthInMbsMinus1 = readUE();
      const picHeightInMapUnitsMinus1 = readUE();
      const frameMbsOnlyFlag = readBit();
      const width = (picWidthInMbsMinus1 + 1) * 16;
      const height =        (2 - frameMbsOnlyFlag) * (picHeightInMapUnitsMinus1 + 1) * 16;
      return { width, height };
    } catch {
      return { width: 1920, height: 1080 };
    }
  }

  private static getCodecFromSPS(sps: Uint8Array): string {
    if (sps.length < 4) return 'avc1.42E01E';
    const profileIdc = sps[1];
    const profileCompatibility = sps[2];
    const levelIdc = sps[3];
    return `avc1.${profileIdc.toString(16).padStart(2, '0')}${profileCompatibility.toString(16).padStart(2, '0')}${levelIdc.toString(16).padStart(2, '0')}`;
  }

  /**
   * Compare two Uint8Arrays for equality
   */
  private static uint8ArrayEquals(a: Uint8Array, b: Uint8Array): boolean {
    if (a.length !== b.length) return false;
    for (let i = 0; i < a.length; i++) {
      if (a[i] !== b[i]) return false;
    }
    return true;
  }

  private sniffCodecIfUnknown(data: Uint8Array): void {
    if (this.videoCodec !== 'unknown' || data.length < 5) return;

    const firstU32 =      ((data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3]) >>> 0;
    const isSingleAvcc = firstU32 > 0 && firstU32 + 4 === data.length;
    if (isSingleAvcc) {
      const b0 = data[4];
      const h264t = b0 & 0x1f;
      const hevcT = (b0 >> 1) & 0x3f;
      if (b0 === 0x67 || b0 === 0x68) {
        this.videoCodec = 'h264';
        return;
      }
      if (hevcT >= 32 && hevcT <= 34) {
        if (h264t >= 1 && h264t <= 5) return;
        if (b0 === 0x40 || b0 === 0x42 || b0 === 0x44 || h264t === 0) {
          this.videoCodec = 'h265';
          return;
        }
      }
    }

    let i = 0;
    while (i + 5 < data.length) {
      if (
        data[i] === 0
        && data[i + 1] === 0
        && data[i + 2] === 0
        && data[i + 3] === 1
      ) {
        const b0 = data[i + 4];
        if (b0 === 0x67 || b0 === 0x68) {
          this.videoCodec = 'h264';
          return;
        }
        const h264t = b0 & 0x1f;
        const hevcT = (b0 >> 1) & 0x3f;
        if (hevcT >= 32 && hevcT <= 34) {
          if (h264t >= 1 && h264t <= 5) {
            i += 4;
            continue;
          }
          if (b0 === 0x40 || b0 === 0x42 || b0 === 0x44 || h264t === 0) {
            this.videoCodec = 'h265';
            return;
          }
        }
      }
      i++;
    }
  }

  private static isHevcVclNal(nalType: number): boolean {
    return (nalType >= 0 && nalType <= 9) || (nalType >= 16 && nalType <= 21);
  }

  private dealHevcVideoData(
    h264Data: Uint8Array,
    isKeyframe: boolean,
    pts90kHz: number
  ): void {
    const pendingAvccChunks: Uint8Array[] = [];
    let auKeyframe = isKeyframe;

    let i = 0;
    while (i < h264Data.length) {
      if (
        i + 3 < h264Data.length
        && h264Data[i] === 0x00
        && h264Data[i + 1] === 0x00
        && h264Data[i + 2] === 0x00
        && h264Data[i + 3] === 0x01
      ) {
        const naluStart = i + 4;
        let j = naluStart;

        while (j < h264Data.length) {
          if (
            j + 3 < h264Data.length
            && h264Data[j] === 0x00
            && h264Data[j + 1] === 0x00
            && h264Data[j + 2] === 0x00
            && h264Data[j + 3] === 0x01
          ) {
            break;
          }
          j++;
        }

        const nalData = h264Data.subarray(naluStart, j);
        if (nalData.length >= 2) {
          const nalType = (nalData[0] >> 1) & 0x3f;

          if (nalType === HEVC_NAL_VPS) {
            this.vpsNalu = new Uint8Array(nalData);
          } else if (nalType === HEVC_NAL_SPS) {
            const newSps = new Uint8Array(nalData);
            if (
              this.spsNalu
              && !H264Player.uint8ArrayEquals(this.spsNalu, newSps)
            ) {
              console.log(
                '[H264Player] HEVC SPS changed, reinitializing MSE decoder'
              );
              this.isInitialized = false;
              this.hevcMuxer = null;
              this.hasReceivedFirstIFrame = false;
              this.nextTimestamp = 0;
              this.firstPts90kHz = undefined;
              this.currentFrame = 0;
              this.ppsNalu = null;
              this.vpsNalu = null;
              if (this.videoPlayer instanceof MsMediaSource) {
                this.videoPlayer.cleanupForNewStream();
              }
            }
            this.spsNalu = newSps;
            const res = parseHevcSpsResolution(this.spsNalu);
            this.spsWidth = res.width;
            this.spsHeight = res.height;
          } else if (nalType === HEVC_NAL_PPS) {
            this.ppsNalu = new Uint8Array(nalData);
          }

          if (
            !this.isInitialized
            && this.vpsNalu
            && this.spsNalu
            && this.ppsNalu
          ) {
            try {
              this.currentFrame = 0;
              this.nextTimestamp = 0;

              const built = createHevcInitSegment(
                this.vpsNalu,
                this.spsNalu,
                this.ppsNalu
              );
              if (!built) {
                console.error(
                  '[H264Player] HEVC init failed or MSE codecs not supported'
                );
                this.cb({ t: 'codecMismatch', codec: 'h265' });
                return;
              }

              this.hevcMuxer = new HevcMp4Muxer();
              this.hevcMuxer.setFrameRate(30);
              this.isInitialized = true;
              this.videoMsgCallback({
                data: {
                  data: built.segment.buffer as ArrayBuffer,
                  codec: built.mimeCodec,
                },
              });
            } catch (e) {
              console.error('[H264Player] Failed to initialize HEVC MSE:', e);
              this.cb({ t: 'codecMismatch', codec: 'h265' });
              return;
            }
          }

          const sliceIsIr = nalType === 19 || nalType === 20;
          const useKeyframe = isKeyframe || sliceIsIr;

          if (
            this.isInitialized
            && this.hevcMuxer
            && H264Player.isHevcVclNal(nalType)
          ) {
            if (!this.hasReceivedFirstIFrame) {
              if (useKeyframe) {
                this.hasReceivedFirstIFrame = true;
              } else {
                i = j;
                continue;
              }
            }

            try {
              auKeyframe = auKeyframe || sliceIsIr;
              const avccData = new Uint8Array(4 + nalData.length);
              avccData[0] = (nalData.length >>> 24) & 0xff;
              avccData[1] = (nalData.length >>> 16) & 0xff;
              avccData[2] = (nalData.length >>> 8) & 0xff;
              avccData[3] = nalData.length & 0xff;
              avccData.set(nalData, 4);
              pendingAvccChunks.push(avccData);
            } catch {
              /* ignore */
            }
          }
        }
        i = j;
      } else {
        i++;
      }
    }

    if (pendingAvccChunks.length > 0 && this.hevcMuxer) {
      try {
        if (this.firstPts90kHz === undefined) {
          this.firstPts90kHz = pts90kHz;
        }

        if (this.lastPts90kHz !== undefined && pts90kHz > this.lastPts90kHz) {
          const ptsDiff = pts90kHz - this.lastPts90kHz;
          if (ptsDiff >= 1500 && ptsDiff <= 18000) {
            const measuredFps = Math.round(90000 / ptsDiff);
            if (
              measuredFps !== this.detectedFrameRate
              && measuredFps >= 5
              && measuredFps <= 60
            ) {
              this.detectedFrameRate = measuredFps;
              this.hevcMuxer?.setFrameRate(measuredFps);
            }
          }
        }
        this.lastPts90kHz = pts90kHz;

        const decodeTime90kHz = this.nextTimestamp;
        this.nextTimestamp += Math.round(90000 / this.detectedFrameRate);

        const relativePts = (pts90kHz - (this.firstPts90kHz as number)) | 0;
        const compositionTimeOffset = relativePts - decodeTime90kHz;

        let totalLen = 0;
        for (const chunk of pendingAvccChunks) totalLen += chunk.length;
        const merged = new Uint8Array(totalLen);
        let off = 0;
        for (const chunk of pendingAvccChunks) {
          merged.set(chunk, off);
          off += chunk.length;
        }
        const mediaSegment = this.hevcMuxer.createMediaSegment(
          merged,
          auKeyframe,
          decodeTime90kHz,
          compositionTimeOffset
        );

        this.currentFrame++;

        this.videoMsgCallback({
          data: {
            data: mediaSegment.buffer as ArrayBuffer,
            codec: '',
          },
        });
      } catch (e) {
        console.error('[H264Player] Error creating HEVC segment:', e);
      }
    }
  }

  dealVideoData(
    frameData: ArrayBuffer,
    isKeyframe: boolean,
    pts90kHz: number,
    _dts90kHz?: number
  ): void {
    const bytes = new Uint8Array(frameData);
    if (bytes.length < 5) return;

    this.sniffCodecIfUnknown(bytes);

    // For WebCodecs, pass AVCC data directly without MP4 encapsulation
    if (
      this.useWebCodecs
      && this.videoPlayer instanceof WebCodecsPlayer
      && this.videoCodec !== 'h265'
    ) {
      this.videoPlayer.processMp4VideoData({
        data: {
          data: frameData,
          codec: '',
        },
      });
      return;
    }

    // Original MSE path below

    const h264Data = new Uint8Array(bytes);
    const pendingAvccChunks: Uint8Array[] = [];

    // Backend sends one NAL per WS message in AVCC format
    // (4-byte big-endian length + NAL body). Detect by checking
    // whether the length field matches the remaining payload.
    const firstU32 =      ((bytes[0] << 24) | (bytes[1] << 16) | (bytes[2] << 8) | bytes[3]) >>> 0;
    const isSingleAVCC = firstU32 > 0 && firstU32 + 4 === bytes.length;

    if (isSingleAVCC) {
      h264Data[0] = 0;
      h264Data[1] = 0;
      h264Data[2] = 0;
      h264Data[3] = 1;
    } else if (
      bytes[0] === 0
      && bytes[1] === 0
      && bytes[2] === 0
      && bytes[3] === 1
    ) {
      // Already Annex-B with 4-byte start code — no conversion needed
    } else {
      // Multi-NAL AVCC or other format — try AVCC conversion
      let offset = 0;
      while (offset + 4 <= h264Data.length) {
        const nalLength =          ((h264Data[offset] << 24)
            | (h264Data[offset + 1] << 16)
            | (h264Data[offset + 2] << 8)
            | h264Data[offset + 3])
          >>> 0;
        if (nalLength === 0 || nalLength > h264Data.length - offset - 4) break;

        h264Data[offset] = 0;
        h264Data[offset + 1] = 0;
        h264Data[offset + 2] = 0;
        h264Data[offset + 3] = 1;

        offset += 4 + nalLength;
      }
    }

    this.sniffCodecIfUnknown(h264Data);

    if (this.videoCodec === 'h265') {
      this.dealHevcVideoData(h264Data, isKeyframe, pts90kHz);
      return;
    }

    // Parse NALUs mimicking videoWorker approach from console
    let i = 0;
    while (i < h264Data.length) {
      if (
        i + 3 < h264Data.length
        && h264Data[i] === 0x00
        && h264Data[i + 1] === 0x00
        && h264Data[i + 2] === 0x00
        && h264Data[i + 3] === 0x01
      ) {
        const naluStart = i + 4;
        let j = naluStart;

        // Find next start code or end of buffer
        while (j < h264Data.length) {
          if (
            j + 3 < h264Data.length
            && h264Data[j] === 0x00
            && h264Data[j + 1] === 0x00
            && h264Data[j + 2] === 0x00
            && h264Data[j + 3] === 0x01
          ) {
            break;
          }
          j++;
        }

        const nalData = h264Data.subarray(naluStart, j);
        if (nalData.length > 0) {
          const nalType = nalData[0] & 0x1f;

          if (nalType === NAL_TYPE_SPS) {
            const newSps = new Uint8Array(nalData);
            // Detect SPS content change (resolution/bitrate/codec parameter change)
            if (
              this.spsNalu
              && !H264Player.uint8ArrayEquals(this.spsNalu, newSps)
            ) {
              // SPS changed — force MSE reinitialization
              console.log(
                '[H264Player] SPS changed, reinitializing MSE decoder'
              );
              this.isInitialized = false;
              this.mp4Muxer = null;
              this.hevcMuxer = null;
              this.hasReceivedFirstIFrame = false;
              this.nextTimestamp = 0;
              this.firstPts90kHz = undefined;
              this.currentFrame = 0;
              this.ppsNalu = null; // Wait for new PPS too
              if (this.videoPlayer instanceof MsMediaSource) {
                this.videoPlayer.cleanupForNewStream();
              }
            }
            this.spsNalu = newSps;
            const res = H264Player.parseSPSResolution(this.spsNalu);
            this.spsWidth = res.width;
            this.spsHeight = res.height;
          } else if (nalType === NAL_TYPE_PPS) {
            this.ppsNalu = new Uint8Array(nalData);
          }

          // Initialization block
          if (!this.isInitialized && this.spsNalu && this.ppsNalu) {
            try {
              // Reset timestamp base on (re)initialization
              this.currentFrame = 0;
              this.nextTimestamp = 0; // For generating continuous timestamps

              // Always create a new muxer on re-initialization
              // This ensures lastTimestamp90kHz is reset for WebSocket reconnects
              this.mp4Muxer = new MP4Muxer();
              this.mp4Muxer.setFrameRate(30);
              const codec = H264Player.getCodecFromSPS(this.spsNalu);
              const initSegment = this.mp4Muxer.createInitSegment(
                this.spsNalu,
                this.ppsNalu,
                this.spsWidth,
                this.spsHeight
              );

              this.isInitialized = true;
              this.videoMsgCallback({
                data: {
                  data: initSegment.buffer as ArrayBuffer,
                  codec: `video/mp4; codecs="${codec}"`,
                },
              });
            } catch (e) {
              console.error('[H264Player] Failed to initialize MSE:', e);
            }
          }

          // Feed Media Segment (filter out SPS/PPS/SEI/AUD)
          if (
            this.isInitialized
            && this.mp4Muxer
            && nalType !== NAL_TYPE_SPS
            && nalType !== NAL_TYPE_PPS
            && nalType !== 6
            && nalType !== 9
          ) {
            // Use isKeyframe from 8-byte header (backend already detected correctly)

            if (!this.hasReceivedFirstIFrame) {
              if (isKeyframe) {
                this.hasReceivedFirstIFrame = true;
              } else {
                // Drop all non-IDR frames until the first IDR frame
                // MSE requires the first media segment appended to be a random access point
                i = j;
                continue;
              }
            }

            try {
              // Standard MP4 requires AVCC format (4-byte length prefix)
              const avccData = new Uint8Array(4 + nalData.length);
              avccData[0] = (nalData.length >>> 24) & 0xff;
              avccData[1] = (nalData.length >>> 16) & 0xff;
              avccData[2] = (nalData.length >>> 8) & 0xff;
              avccData[3] = nalData.length & 0xff;
              avccData.set(nalData, 4);

              // Batch: collect AVCC data for the current access unit;
              // generate the fMP4 segment after the NALU loop finishes.
              pendingAvccChunks.push(avccData);
            } catch {
              // ignore segment errors
            }
          }
        }
        i = j;
      } else {
        i++;
      }
    }

    // Flush batched NALs as a single fMP4 segment
    if (pendingAvccChunks.length > 0 && this.mp4Muxer) {
      try {
        // 锁定首帧 PTS 作为归一化基准
        if (this.firstPts90kHz === undefined) {
          this.firstPts90kHz = pts90kHz;
        }

        // Detect frame rate from PTS differences
        if (this.lastPts90kHz !== undefined && pts90kHz > this.lastPts90kHz) {
          const ptsDiff = pts90kHz - this.lastPts90kHz;
          // Only update if PTS diff is reasonable (5-60 fps range: 1500-18000 ticks@90kHz)
          if (ptsDiff >= 1500 && ptsDiff <= 18000) {
            const measuredFps = Math.round(90000 / ptsDiff);
            if (
              measuredFps !== this.detectedFrameRate
              && measuredFps >= 5
              && measuredFps <= 60
            ) {
              this.detectedFrameRate = measuredFps;
              this.mp4Muxer?.setFrameRate(measuredFps);
            }
          }
        }
        this.lastPts90kHz = pts90kHz;

        // DTS = 前端独立的单调帧计数器（忽略后端 DTS，避免管道不一致）
        const decodeTime90kHz = this.nextTimestamp;
        this.nextTimestamp += Math.round(90000 / this.detectedFrameRate); // dynamic fps

        // 相对 PTS：uint32 减法（| 0 强制 int32）天然支持 wrap-around，
        // 只要 GOP 内 PTS 差值 << 2^31
        const relativePts = (pts90kHz - this.firstPts90kHz) | 0;

        // CTO 可为负值（B 帧 PTS < DTS 时），由 trun v1 承载有符号值
        const compositionTimeOffset = relativePts - decodeTime90kHz;

        // Concatenate all AVCC chunks into one sample
        let totalLen = 0;
        for (const chunk of pendingAvccChunks) totalLen += chunk.length;
        const merged = new Uint8Array(totalLen);
        let off = 0;
        for (const chunk of pendingAvccChunks) {
          merged.set(chunk, off);
          off += chunk.length;
        }
        const mediaSegment = this.mp4Muxer.createMediaSegment(
          merged,
          isKeyframe,
          decodeTime90kHz,
          compositionTimeOffset
        );

        this.currentFrame++;

        this.videoMsgCallback({
          data: {
            data: mediaSegment.buffer as ArrayBuffer,
            codec: '',
          },
        });
      } catch (e) {
        console.error('[H264Player] Error creating/segment:', e);
      }
    }
  }

  stopPlay(): ReturnResult {
    const ret = {
      e: 0,
      m: 'Stop h264 Success',
    };

    this.clearBuffer();
    this.videoPlayer?.uninitMse();
    this.videoPlayer = null;
    if (this.type === 'audio') {
      if (this.videoElement) {
        document.body.removeChild(this.videoElement);
        this.videoElement = null;
      }
    }
    this.type = 'video';

    // Reset Muxer state
    this.isInitialized = false;
    this.mp4Muxer = null;
    this.hevcMuxer = null;
    this.hasReceivedFirstIFrame = false;
    this.spsNalu = null;
    this.ppsNalu = null;
    this.vpsNalu = null;
    this.videoCodec = 'unknown';
    this.detectedFrameRate = 30;
    this.lastPts90kHz = undefined;
    this.nextTimestamp = 0;
    this.firstPts90kHz = undefined;
    this.currentFrame = 0;
    return ret;
  }

  displayLoop(opt: number): void {
    if (opt) {
      requestAnimationFrame(this.displayLoop.bind(this));
    }
    // updateSourceBuffer is MSE-specific, WebCodecs doesn't need it
    if (this.videoPlayer instanceof MsMediaSource) {
      this.videoPlayer.updateSourceBuffer();
    }
  }

  static setPlayMode(_opt: boolean): void {
    // Obsolete
  }

  /**
   * Get buffered time (how much video is buffered ahead of current playback position)
   */
  private getBufferedTime(): number {
    if (!this.videoElement || this.videoElement.buffered.length === 0) return 0;
    return (
      this.videoElement.buffered.end(this.videoElement.buffered.length - 1)
      - this.videoElement.currentTime
    );
  }

  /**
   * Calculate adaptive buffer threshold based on rolling average
   */

  /**
   * Calculate latency (how far behind live we are)
   */
  private calculateLatency(): number {
    if (!this.videoElement) return 0;

    const { seekable } = this.videoElement;
    if (seekable.length > 0) {
      return Math.max(
        0,
        seekable.end(seekable.length - 1) - this.videoElement.currentTime
      );
    }

    // Fallback: use buffered range
    const { buffered } = this.videoElement;
    if (buffered.length > 0) {
      return Math.max(
        0,
        buffered.end(buffered.length - 1) - this.videoElement.currentTime
      );
    }

    return 0;
  }

  /**
   * Calculate bandwidth in kBps
   */
  private calculateBandwidth(): void {
    const now = Date.now();
    const timeElapsed = (now - this.lastBandwidthCheck) / 1000; // seconds

    if (timeElapsed >= 1) {
      const bytesLoaded = this.totalBytesLoaded;
      this.bandwidth =        (bytesLoaded - this.lastBytesLoaded) / timeElapsed / 1000; // kBps

      this.lastBytesLoaded = bytesLoaded;
      this.lastBandwidthCheck = now;
    }
  }

  /**
   * Setup progress handler for adaptive buffer management
   */
  private setupProgressHandler(): void {
    if (!this.videoElement) return;

    this.videoElement.addEventListener('progress', () => {
      this.onProgress();
    });

    if (this.statsIntervalId) {
      clearInterval(this.statsIntervalId);
    }
    this.statsIntervalId = setInterval(() => {
      this.calculateBandwidth();
      this.latency = this.calculateLatency();
    }, 1000);
  }

  /**
   * Handle progress events
   *
   * Strategy: Latency chase and buffer trimming are handled by media.ts.
   * This handler only updates latency stats.
   */
  private onProgress(): void {
    if (!this.videoElement) return;

    // Update latency
    this.latency = this.calculateLatency();
  }

  /**
   * Seek to live edge (only used for extreme lag recovery, e.g. tab return).
   * Normal latency chase is handled by media.ts debounced seek strategy.
   */
  private seekToLiveEdge(): void {
    if (!this.videoElement) return;

    const { buffered } = this.videoElement;
    if (buffered.length === 0) return;

    const liveEdge = buffered.end(buffered.length - 1);
    const lag = liveEdge - this.videoElement.currentTime;
    if (lag > 1.5) {
      this.videoElement.currentTime = liveEdge - 0.1;
    }
  }

  /**
   * Get player statistics
   */
  public getStats(): {
    bandwidth: number;
    latency: number;
    bufferTime: number;
    playbackRate: number;
    packetsPerSecond: number;
    detectedFrameRate: number;
  } {
    return {
      bandwidth: this.bandwidth,
      latency: this.latency,
      bufferTime: this.getBufferedTime(),
      playbackRate: this.videoElement?.playbackRate ?? 1,
      packetsPerSecond: this.packetsPerSecond,
      detectedFrameRate: this.detectedFrameRate,
    };
  }
}
