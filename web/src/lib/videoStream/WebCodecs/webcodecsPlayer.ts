/**
 * WebCodecs H264 Player
 *
 * Uses the WebCodecs API for direct H264 decoding without MP4 encapsulation.
 * Falls back to MSE player if WebCodecs is not supported.
 *
 * Advantages over MSE:
 * - No MP4 muxing overhead
 * - Direct frame-level control
 * - Better error recovery
 * - Can detect and handle frame drops explicitly
 */

interface CallbackEvent {
  t: 'mseError' | 'startPlay';
}

type CallbackFunction = (event: CallbackEvent) => void;

interface Mp4EventData {
  data: ArrayBuffer;
  codec: string;
}

interface NALUnit {
  data: Uint8Array;
  avccData: Uint8Array;
  type: number;
  isKeyframe: boolean;
}

/* eslint-disable class-methods-use-this */
// Check if WebCodecs is supported
export function isWebCodecsSupported(): boolean {
  return (
    typeof VideoDecoder !== 'undefined'
    && typeof VideoFrame !== 'undefined'
    && typeof EncodedVideoChunk !== 'undefined'
  );
}

class WebCodecsPlayer {
  private decoder: VideoDecoder | null = null;

  private canvas: HTMLCanvasElement | null = null;

  private ctx: CanvasRenderingContext2D | null = null;

  private cb: CallbackFunction;

  private codec: string = '';

  private sps: Uint8Array | null = null;

  private pps: Uint8Array | null = null;

  private isInitialized: boolean = false;

  private frameCount: number = 0;

  private isActive: boolean = true;

  private hasDecodedKeyframe: boolean = false;

  private videoElement: HTMLVideoElement | null = null;

  private previousVideoVisibility: string | null = null;

  private objectFit: 'contain' | 'cover' = 'contain';

  private frameWidth: number = 0;

  private frameHeight: number = 0;

  constructor(cb: CallbackFunction) {
    this.cb = cb;
  }

  /**
   * Initialize the WebCodecs decoder
   */
  async initMse(codecString: string): Promise<boolean> {
    // console.log('[WebCodecs] initMse called, codec:', codecString);

    if (!isWebCodecsSupported()) {
      console.error('[WebCodecs] Not supported in this browser');
      return false;
    }

    this.codec = codecString;
    return true;
  }

  /**
   * Set the canvas element for rendering
   */
  setVideoElement(videoElement: HTMLVideoElement): void {
    this.videoElement = videoElement;
    this.isActive = true;

    if (this.canvas?.parentNode) {
      this.canvas.remove();
    }

    // Create a canvas to render decoded frames
    this.canvas = document.createElement('canvas');
    this.canvas.width = 1920;
    this.canvas.height = 1080;
    const computedStyle = window.getComputedStyle(videoElement);
    this.objectFit = computedStyle.objectFit === 'cover' ? 'cover' : 'contain';
    this.canvas.style.position = 'absolute';
    this.canvas.style.inset = '0';
    this.canvas.style.width = '100%';
    this.canvas.style.height = '100%';
    this.canvas.style.display = 'block';
    this.canvas.style.pointerEvents = 'none';
    this.ctx = this.canvas.getContext('2d', { alpha: false });

    // Keep the video element in the DOM so MSE fallback can re-use it after a
    // WebCodecs decode error. The canvas is just a rendering layer above it.
    this.previousVideoVisibility = videoElement.style.visibility;
    videoElement.style.visibility = 'hidden';
    if (videoElement.parentElement) {
      videoElement.parentElement.appendChild(this.canvas);
    }
  }

  /**
   * Process H264 NAL units and decode them
   */
  processMp4VideoData(event: { data: Mp4EventData }): void {
    if (!this.isActive) return;

    const objData = event.data;
    const data = new Uint8Array(objData.data);

    // Parse NAL units from AVCC format
    const nals = this.parseAVCCNALs(data);

    // Extract SPS/PPS and initialize decoder
    for (const nal of nals) {
      const nalType = nal.type;

      if (nalType === 7) {
        // SPS
        this.sps = nal.data;
        // console.log('[WebCodecs] Received SPS, size:', nal.data.length);
      } else if (nalType === 8) {
        // PPS
        this.pps = nal.data;
        // console.log('[WebCodecs] Received PPS, size:', nal.data.length);
      }
    }

    // Initialize decoder when we have SPS and PPS
    if (!this.isInitialized && this.sps && this.pps) {
      this.initializeDecoder();
    }

    // Decode video NALs (exclude SPS/PPS/AUD/SEI)
    if (this.isInitialized && this.decoder) {
      const frameNals = nals.filter(nal => nal.type >= 1 && nal.type <= 5);
      if (frameNals.length > 0) {
        this.decodeAccessUnit(frameNals);
      }
    }
  }

  /**
   * Parse NAL units from AVCC format (4-byte length prefix)
   */
  // eslint-disable-next-line class-methods-use-this
  private parseAVCCNALs(data: Uint8Array): NALUnit[] {
    const nals: NALUnit[] = [];
    let offset = 0;

    while (offset + 4 <= data.length) {
      const length =        (data[offset] << 24)
        | (data[offset + 1] << 16)
        | (data[offset + 2] << 8)
        | data[offset + 3];

      if (length <= 0 || offset + 4 + length > data.length) {
        break;
      }

      const nalData = data.subarray(offset + 4, offset + 4 + length);
      const nalType = nalData[0] & 0x1f;

      nals.push({
        data: nalData,
        avccData: data.subarray(offset, offset + 4 + length),
        type: nalType,
        isKeyframe: nalType === 5, // IDR frame
      });

      offset += 4 + length;
    }

    return nals;
  }

  /**
   * Initialize the VideoDecoder
   */
  private initializeDecoder(): void {
    if (!this.sps || !this.pps) return;

    try {
      // Generate codec string from SPS if not set
      if (!this.codec || this.codec === '') {
        const profileIdc = this.sps[1];
        const profileCompatibility = this.sps[2];
        const levelIdc = this.sps[3];
        this.codec = `avc1.${profileIdc.toString(16).padStart(2, '0')}${profileCompatibility.toString(16).padStart(2, '0')}${levelIdc.toString(16).padStart(2, '0')}`;
      }

      // Create codec description in AVCC format
      const description = this.createCodecDescription(this.sps, this.pps);

      const config: VideoDecoderConfig = {
        codec: this.codec,
        description,
        optimizeForLatency: true,
      };

      console.log('[WebCodecs] Initializing decoder with codec:', this.codec);

      this.decoder = new VideoDecoder({
        output: (frame: VideoFrame) => {
          this.renderFrame(frame);
        },
        error: (error: Error) => {
          console.error('[WebCodecs] Decoder error:', error);
          // Notify parent to fall back to MSE
          this.cb({ t: 'mseError' });
        },
      });

      try {
        this.decoder.configure(config);
        // Check if configuration was successful
        if (this.decoder.state === 'configured') {
          // console.log('[WebCodecs] Decoder configured successfully');
          this.isInitialized = true;
          this.cb({ t: 'startPlay' });
        } else {
          console.error('[WebCodecs] Decoder failed to configure');
          this.handleDecoderError();
        }
      } catch (configureErr) {
        console.error('[WebCodecs] Exception during configure:', configureErr);
        this.handleDecoderError();
      }
    } catch (e) {
      console.error('[WebCodecs] Failed to initialize decoder:', e);
      this.handleDecoderError();
    }
  }

  /**
   * Create codec description from SPS/PPS in AVCC format
   */
  // eslint-disable-next-line class-methods-use-this
  private createCodecDescription(sps: Uint8Array, pps: Uint8Array): Uint8Array {
    // AVCC format:
    // - configuration version (1 byte)
    // - profile, level, compatibility (3 bytes)
    // - length size minus one (1 byte, usually 3 = 4 bytes)
    // - number of SPS (1 byte)
    // - SPS length (2 bytes) + SPS data
    // - number of PPS (1 byte)
    // - PPS length (2 bytes) + PPS data

    const result = new Uint8Array(
      1 + 3 + 1 + 1 + 2 + sps.length + 1 + 2 + pps.length
    );
    let offset = 0;

    // Configuration version
    result[offset++] = 1;

    // Profile, profile compatibility, level
    /* eslint-disable prefer-destructuring */
    result[offset++] = sps[1];
    result[offset++] = sps[2];
    result[offset++] = sps[3];
    /* eslint-enable prefer-destructuring */

    // Length size minus one (4 bytes)
    result[offset++] = 0xff;

    // Number of SPS
    result[offset++] = 0xe1; // 1 SPS with high bits set

    // SPS length
    result[offset++] = (sps.length >> 8) & 0xff;
    result[offset++] = sps.length & 0xff;

    // SPS data
    result.set(sps, offset);
    offset += sps.length;

    // Number of PPS
    result[offset++] = 1;

    // PPS length
    result[offset++] = (pps.length >> 8) & 0xff;
    result[offset++] = pps.length & 0xff;

    // PPS data
    result.set(pps, offset);

    return result;
  }

  /**
   * Decode one access unit. With an AVCC decoder config, WebCodecs expects
   * length-prefixed NAL units in each EncodedVideoChunk.
   */
  private decodeAccessUnit(nals: NALUnit[]): void {
    if (!this.decoder || this.decoder.state !== 'configured') {
      return;
    }

    const isKeyframe = nals.some(nal => nal.isKeyframe);
    if (!this.hasDecodedKeyframe) {
      if (!isKeyframe) return;
      this.hasDecodedKeyframe = true;
    }

    let totalLength = 0;
    for (const nal of nals) totalLength += nal.avccData.length;
    const data = new Uint8Array(totalLength);
    let offset = 0;
    for (const nal of nals) {
      data.set(nal.avccData, offset);
      offset += nal.avccData.length;
    }

    try {
      // Generate timestamp in microseconds (90kHz * 1000 / 90 = microseconds)
      const timestamp = this.frameCount * 33333; // ~30fps
      this.frameCount++;

      const chunk = new EncodedVideoChunk({
        type: isKeyframe ? 'key' : 'delta',
        timestamp,
        data,
      });

      this.decoder.decode(chunk);
    } catch (e) {
      console.error('[WebCodecs] Failed to decode chunk:', e);
      this.handleDecoderError();
    }
  }

  /**
   * Render decoded frame to canvas
   */
  private renderFrame(frame: VideoFrame): void {
    if (!this.ctx || !this.canvas || !this.isActive) {
      frame.close();
      return;
    }

    const cssW = this.canvas.clientWidth
      || this.canvas.parentElement?.clientWidth
      || frame.displayWidth;
    const cssH = this.canvas.clientHeight
      || this.canvas.parentElement?.clientHeight
      || frame.displayHeight;
    const dpr = window.devicePixelRatio || 1;
    const targetW = Math.max(1, Math.round(cssW * dpr));
    const targetH = Math.max(1, Math.round(cssH * dpr));

    if (this.canvas.width !== targetW || this.canvas.height !== targetH) {
      this.canvas.width = targetW;
      this.canvas.height = targetH;
    }

    if (
      this.videoElement
      && (this.frameWidth !== frame.displayWidth
        || this.frameHeight !== frame.displayHeight)
    ) {
      this.frameWidth = frame.displayWidth;
      this.frameHeight = frame.displayHeight;
      this.videoElement.dispatchEvent(
        new CustomEvent('aipc:video-frame-size', {
          detail: { width: frame.displayWidth, height: frame.displayHeight },
        })
      );
    }

    this.ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    this.ctx.fillStyle = 'black';
    this.ctx.fillRect(0, 0, cssW, cssH);

    const scale = this.objectFit === 'cover'
        ? Math.max(cssW / frame.displayWidth, cssH / frame.displayHeight)
        : Math.min(cssW / frame.displayWidth, cssH / frame.displayHeight);
    const drawW = frame.displayWidth * scale;
    const drawH = frame.displayHeight * scale;
    const drawX = (cssW - drawW) / 2;
    const drawY = (cssH - drawH) / 2;

    this.ctx.drawImage(frame, drawX, drawY, drawW, drawH);

    frame.close();

    // Auto-play notification on first frame
    if (this.frameCount === 1) {
      // console.log('[WebCodecs] First frame rendered');
      this.cb({ t: 'startPlay' });
    }
  }

  /**
   * Handle decoder errors
   */
  private handleDecoderError(): void {
    console.warn('[WebCodecs] Decoder error, triggering MSE fallback');
    this.cb({ t: 'mseError' });
  }

  /**
   * Uninitialize and cleanup
   */
  uninitMse(): void {
    this.isActive = false;

    if (this.decoder) {
      try {
        if (this.decoder.state === 'configured') {
          this.decoder.close();
        }
      } catch {
        // Ignore close errors
      }
      this.decoder = null;
    }

    if (this.ctx) {
      this.ctx = null;
    }

    if (this.canvas && this.canvas.parentNode) {
      this.canvas.remove();
    }

    this.canvas = null;
    if (this.videoElement && this.previousVideoVisibility !== null) {
      this.videoElement.style.visibility = this.previousVideoVisibility;
    }
    this.previousVideoVisibility = null;
    this.sps = null;
    this.pps = null;
    this.isInitialized = false;
    this.frameCount = 0;
    this.hasDecodedKeyframe = false;
  }

  /**
   * Clear buffer (no-op for WebCodecs as we don't buffer)
   */
  clearBuffer(): void {
    // WebCodecs doesn't maintain a buffer like MSE
  }

  setPlayMode(_playback: boolean): void {
    // No-op for WebCodecs
  }

  resetInitFlag(): void {
    // No-op for WebCodecs
  }

  getInitFlag(): number {
    return this.isInitialized ? 2 : 0; // statusNormal : statusIdel
  }
}

export default WebCodecsPlayer;
