// @ts-nocheck
/**
 * MSE 管道：EncodedFrame(AnnexB) → fMP4 remux → MediaSource → <video> 硬解。
 * H.264/H.265 双支持；SPS 变更整链路重建；H.265 MIME 探测失败 → CODEC_MISMATCH
 * （码流级降级信号，由 FallbackMachine 决策）。
 */

import type { EncodedFrame } from '../shared/types';
import type { Pipeline, PipelineContext, PipelineStats } from './types';
import { MseSource } from './mse-source';
import { H264Muxer } from '../media/fmp4/h264-muxer';
import { HevcMuxer, createHevcInitSegment } from '../media/fmp4/hevc-muxer';
import { MediaClock } from '../media/clock';
import { h264CodecString, parseH264SpsCodedSize } from '../media/sps-h264';
import { bytesEqual, frameToAvcc, isH264Vcl, isHevcVcl } from '../media/nalu';
import { createLogger } from '../shared/log';

const log = createLogger('mse-pipeline');
const RECOVERY_DELAY_MS = 300;
const RECOVERY_WINDOW_MS = 60_000;
const MAX_RECOVERIES = 3;

/** Constant work per indexed NAL; this does not validate the encoded slice payload. */
function validFrameHeaders(frame: EncodedFrame): boolean {
  if (frame.nalus.length === 0) return false;
  for (const n of frame.nalus) {
    const headerSize = frame.codec === 'h264' ? 1 : 2;
    if (!Number.isSafeInteger(n.offset) || !Number.isSafeInteger(n.size) || n.offset < 0 || n.size < 4 + headerSize || n.offset + n.size > frame.data.length) return false;
    const at = n.offset;
    if (frame.data[at] !== 0 || frame.data[at + 1] !== 0 || frame.data[at + 2] !== 0 || frame.data[at + 3] !== 1) return false;
    const first = frame.data[at + 4];
    if (first & 0x80) return false;
    const actualType = frame.codec === 'h264' ? first & 0x1f : (first >> 1) & 0x3f;
    if (actualType !== n.type || (frame.codec === 'h264' && actualType === 0)) return false;
    if (frame.codec === 'h265' && (frame.data[at + 5] & 7) === 0) return false;
    // An AVC SPS must contain the profile, constraints and level bytes.
    if (frame.codec === 'h264' && actualType === 7 && n.size < 8) return false;
    const needsPayload = frame.codec === 'h264' ? isH264Vcl(actualType) || actualType === 7 || actualType === 8 : isHevcVcl(actualType) || (actualType >= 32 && actualType <= 34);
    if (needsPayload && n.size === 4 + headerSize) return false;
  }
  return true;
}

export class MsePipeline implements Pipeline {
  readonly tech = 'mse';

  private source: MseSource | null = null;
  private muxer: H264Muxer | HevcMuxer | null = null;
  private clock = new MediaClock();
  private codec: EncodedFrame['codec'] | null = null;
  private sps: Uint8Array | null = null;
  private pps: Uint8Array | null = null;
  private vps: Uint8Array | null = null;
  private initialized = false;
  private gotKeyframe = false;
  private lastFps = 30;
  private statsData: PipelineStats = { framesIn: 0, framesOut: 0, dropped: 0 };
  private recoveryTimer: ReturnType<typeof setTimeout> | null = null;
  private recoveryTimes: number[] = [];

  constructor(private readonly ctx: PipelineContext) {}

  init(): void {
    const video = this.ctx.render.getVideo();
    if (!video) {
      this.ctx.onFatal('RENDER_INIT_FAILED', 'no video element');
      return;
    }
    this.ctx.render.useSurface('video');
    this.source = new MseSource(
      video,
      this.ctx.latency,
      {
        onFatal: (code, detail) => {
          if (code === 'DECODE_ERROR' && this.ctx.mode !== 'playback') this.recover(detail);
          else this.ctx.onFatal(code, detail);
        },
        onStarted: () => this.ctx.onReady(),
      },
      this.ctx.mode === 'playback'
    );
  }

  submit(frame: EncodedFrame): void {
    this.statsData.framesIn += 1;
    const source = this.source;
    if (!source) return;
    if (this.recoveryTimer !== null) { this.statsData.dropped += 1; return; }
    if (!validFrameHeaders(frame)) {
      this.statsData.dropped += 1;
      // An invalid VCL may be a reference for later frames; resume at a random access point.
      this.gotKeyframe = false;
      return;
    }
    if (this.codec === null) this.codec = frame.codec;
    if (frame.codec !== this.codec) return; // 码流切换由 Player 层重建管道

    // 参数集提取（基于索引，无重复扫描）。同时收齐本帧的候选参数集：
    // SPS 变更触发重建时旧 PPS/VPS 已不匹配，必须整体换用同帧随行的
    // 新参数集，否则会跳过携带变更的干净 IDR，冻结到参数集重发为止。
    let newSps: Uint8Array | null = null;
    let newPps: Uint8Array | null = null;
    let newVps: Uint8Array | null = null;
    for (let i = 0; i < frame.nalus.length; i++) {
      const n = frame.nalus[i];
      const type = n.type;
      const payload = frame.data.subarray(n.offset + 4, n.offset + n.size);
      if (this.codec === 'h264') {
        if (type === 7 && !newSps) newSps = payload;
        else if (type === 8 && !newPps) newPps = payload;
      } else {
        if (type === 32 && !newVps) newVps = payload;
        else if (type === 33 && !newSps) newSps = payload;
        else if (type === 34 && !newPps) newPps = payload;
      }
    }

    // SPS 内容变化（分辨率/编码参数变更）→ 整链路重建
    if (newSps) {
      if (this.sps && !bytesEqual(this.sps, newSps)) {
        log.info('SPS changed, rebuilding MSE chain');
        this.initialized = false;
        this.gotKeyframe = false;
        this.muxer = null;
        this.pps = null;
        this.vps = null;
        this.clock.reset();
        this.lastFps = 30;
        source.reset();
      }
      this.sps = newSps;
    }
    if (newPps) this.pps = newPps;
    if (newVps) this.vps = newVps;

    // 参数集齐备 → 建 init segment
    if (!this.initialized && this.sps && this.pps) {
      if (this.codec === 'h264') {
        const dims = parseH264SpsCodedSize(this.sps) ?? { width: 1920, height: 1080 };
        const mime = `video/mp4; codecs="${h264CodecString(this.sps)}"`;
        if (!source.open(mime)) {
          this.ctx.onFatal('CONFIGURE_FAILED', `open MSE failed: ${mime}`);
          return;
        }
        const muxer = new H264Muxer();
        this.muxer = muxer;
        source.append(muxer.createInitSegment(this.sps, this.pps, dims.width, dims.height));
        this.initialized = true;
      } else if (this.vps && this.sps && this.pps) {
        const built = createHevcInitSegment(this.vps, this.sps, this.pps);
        if (!built) {
          // 真实 SPS PTL 探测失败：本机 MSE 解不了这条流 → 码流级降级
          log.warn('HEVC init failed (PTL probe or MIME unsupported)');
          this.ctx.onFatal('CODEC_MISMATCH', { codec: 'h265' });
          return;
        }
        if (!source.open(built.mimeCodec)) {
          this.ctx.onFatal('CONFIGURE_FAILED', `open MSE failed: ${built.mimeCodec}`);
          return;
        }
        const muxer = new HevcMuxer();
        this.muxer = muxer;
        source.append(built.segment);
        this.initialized = true;
      }
    }

    // 媒体片段
    if (this.recoveryTimer !== null) return;
    const hasVcl = frame.nalus.some((n) =>
      this.codec === 'h264' ? isH264Vcl(n.type) : isHevcVcl(n.type)
    );
    if (!this.initialized || !this.muxer || !hasVcl) return;

    if (!this.gotKeyframe) {
      if (!frame.keyframe) {
        this.statsData.dropped += 1; // 首个 IDR 前丢弃（MSE 要求随机访问点起步）
        return;
      }
      this.gotKeyframe = true;
    }

    const timing = this.clock.next(frame.pts90k);
    if (timing.fps !== this.lastFps) {
      this.muxer.setFrameRate(timing.fps);
      this.lastFps = timing.fps;
    }
    // 样本过滤：h264 去 SPS/PPS/SEI/AUD；h265 仅 VCL
    const filter =
      this.codec === 'h264'
        ? (t: number) => t !== 7 && t !== 8 && t !== 6 && t !== 9
        : (t: number) => isHevcVcl(t);
    const sample = frameToAvcc(frame, filter);
    if (sample.length === 0) return;
    const segment = this.muxer.createMediaSegment(sample, frame.keyframe, timing.dts, timing.cto);
    source.append(segment);
    this.statsData.framesOut += 1;
  }

  suspend(): void {
    this.source?.suspend();
  }

  resume(): void {
    this.source?.resume();
  }

  stats(): PipelineStats {
    return { ...this.statsData };
  }

  latencyInfo(): { latencyMs: number | null; bufferedMs: number | null } {
    return this.source?.latencyInfo() ?? { latencyMs: null, bufferedMs: null };
  }

  seek(sec: number): void {
    // NaN = 跳 live edge（A/V 同步与直播恢复共用语义）
    if (!Number.isFinite(sec)) {
      this.source?.seekToLiveEdge();
      return;
    }
    this.source?.seek(sec);
  }

  setSpeed(rate: number): void {
    this.source?.setSpeed(rate);
  }

  private recover(detail?: unknown): void {
    if (this.recoveryTimer !== null || !this.source) return;
    const now = Date.now();
    this.recoveryTimes = this.recoveryTimes.filter(time => now - time < RECOVERY_WINDOW_MS);
    if (this.recoveryTimes.length >= MAX_RECOVERIES) {
      this.ctx.onFatal('DECODE_ERROR', detail);
      return;
    }
    this.recoveryTimes.push(now);
    this.gotKeyframe = false;
    log.warn('MSE playback failed; rebuilding and waiting for a keyframe', detail);
    this.recoveryTimer = setTimeout(() => {
      this.recoveryTimer = null;
      if (!this.source) return;
      this.source.reset();
      this.initialized = false;
      this.muxer = null;
      this.clock.reset();
      this.lastFps = 30;
    }, RECOVERY_DELAY_MS);
  }

  timeline(): { durationMs: number | null; positionMs: number; ranges: Array<[number, number]> } {
    return this.source?.timeline() ?? { durationMs: null, positionMs: 0, ranges: [] };
  }

  destroy(): void {
    if (this.recoveryTimer !== null) clearTimeout(this.recoveryTimer);
    this.recoveryTimer = null;
    this.source?.destroy();
    this.source = null;
    this.muxer = null;
    this.sps = null;
    this.pps = null;
    this.vps = null;
    this.initialized = false;
    this.gotKeyframe = false;
  }
}
