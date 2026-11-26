/**
 * MediaClock：全库唯一时间源（P6）。
 *
 * 时间戳策略（移植 ne301 getAdaptivePreviewFrameDuration 的结论）：
 * - 设备 PTS 存在**细粒度**差值（连续 ≥2 次落在 1–60fps 窗口）→ 采信：ptsRelative/cto 用真实值
 *   （B 帧/解码序乱序流必需）；帧率仅在差值**连续一致**时才交给 PTS——
 *   解码序乱序流（Hailo 系按解码序推流、PTS 为显示序）的到达序差值正负交错，
 *   单差值测得的帧率是噪声，此时帧率由到达节奏 EWMA 供数（内容 20fps 打在
 *   30fps PTS 网格上的实测形态）。
 * - 设备 PTS 粗粒度/缺失（实测设备同一秒内所有帧 PTS 相同）→ 丢弃 PTS 信息，
 *   改按**到达节奏**实测帧率（单帧间隔 EWMA 平滑），dts 计数合成、cto=0。
 *   用错误粒度的 PTS 驱动时间轴会让 MSE 以 1.7x 速度耗尽缓冲 → 周期性停顿。
 */

export interface FrameTiming {
  /** 单调递增解码时间（90kHz） */
  dts: number;
  /** 相对展示时间（90kHz）；PTS 不可信时恒等于 dts */
  ptsRelative: number;
  /** composition time offset = ptsRelative - dts（B 帧可为负；合成时为 0） */
  cto: number;
  /** 当前估计帧率 */
  fps: number;
}

// 下限 1fps 对齐媒体设置的帧率范围（1–30fps）：更低的门限会把 3–4fps 低帧率
// 流的合法 PTS 判成粗粒度，合成轨道又钳在原 FPS_MIN 上 → 呈现过快、抖动。
// 粗粒度 PTS（同秒同值）的零差值仍走 else 分支重置 streak，不会被放宽误伤。
const FPS_MIN = 1;
const FPS_MAX = 60;
/** 1–60fps 对应的 PTS 差值窗口（90kHz ticks） */
const PTS_DIFF_MIN = 1500;
const PTS_DIFF_MAX = 90000;
/** 到达间隔合理窗口（ms）：2–66fps；窗口外的间隔（暂停/网络空洞）不计入估计 */
const ARRIVAL_DT_MIN = 15;
const ARRIVAL_DT_MAX = 500;
/** 采信 PTS 所需的连续有效差值次数 */
const PTS_TRUST_THRESHOLD = 2;
/** PTS 差值帧率采信所需的连续一致次数 */
const PTS_FPS_STREAK = 2;

export interface MediaClockOptions {
  initialFps?: number;
  now?: () => number;
}

export class MediaClock {
  private detectedFps: number;
  private readonly now: () => number;

  private lastPts: number | undefined;
  private firstPts: number | undefined;
  private nextDts = 0;
  private validPtsDiffs = 0;
  private ptsTrusted = false;
  /** PTS 差值连续一致的测定帧率（解码序乱序流的差值是噪声，见下方门控） */
  private lastMeasuredFps: number | null = null;
  private fpsStreak = 0;
  private fpsFromPts = false;
  private ptsBacktracks = 0;

  // 到达节奏帧率估计（EWMA）
  private ewmaFps: number | null = null;
  private lastArrivalMs: number | null = null;
  private arrivals: number[] = [];

  constructor(opts: MediaClockOptions = {}) {
    this.detectedFps = opts.initialFps ?? 30;
    this.now = opts.now ?? (() => Date.now());
  }

  reset(): void {
    this.lastPts = undefined;
    this.firstPts = undefined;
    this.nextDts = 0;
    this.detectedFps = 30;
    this.validPtsDiffs = 0;
    this.ptsTrusted = false;
    this.lastMeasuredFps = null;
    this.fpsStreak = 0;
    this.fpsFromPts = false;
    this.ptsBacktracks = 0;
    this.ewmaFps = null;
    this.lastArrivalMs = null;
    this.arrivals = [];
  }

  get fps(): number {
    return this.detectedFps;
  }

  /** 每个访问单元调用一次。 */
  next(pts90k?: number): FrameTiming {
    // —— 到达节奏测量（始终进行，供 PTS 不可信时使用） ——
    const nowMs = this.now();
    if (this.lastArrivalMs !== null && (nowMs < this.lastArrivalMs || nowMs - this.lastArrivalMs > 1000)) this.arrivals = [];
    this.arrivals.push(nowMs);
    while (this.arrivals.length > 2 && nowMs - this.arrivals[0] > 2000) this.arrivals.shift();
    if (this.arrivals.length > 256) this.arrivals.shift();
    if (this.lastArrivalMs !== null) {
      const dt = nowMs - this.lastArrivalMs;
      if (dt >= ARRIVAL_DT_MIN && dt <= ARRIVAL_DT_MAX) {
        const inst = 1000 / dt;
        this.ewmaFps = this.ewmaFps === null ? inst : this.ewmaFps * 0.75 + inst * 0.25;
      }
    }
    this.lastArrivalMs = nowMs;
    const arrivalSpan = nowMs - this.arrivals[0];
    const windowFps = arrivalSpan >= 500 ? (this.arrivals.length - 1) * 1000 / arrivalSpan : null;

    // —— PTS 可信性判定与帧率来源选择 ——
    if (pts90k !== undefined) {
      if (this.firstPts === undefined) this.firstPts = pts90k;
      if (this.lastPts !== undefined) {
        // The protocol timestamp may wrap at 2^32; interpret short deltas modulo 2^32.
        const diff = (pts90k - this.lastPts) | 0;
        if (diff < 0) {
          this.ptsBacktracks++;
          this.fpsFromPts = false;
        }
        // Presentation order may differ from decode order. A negative interval
        // with a useful magnitude is still evidence of a fine-grained PTS.
        if (Math.abs(diff) >= PTS_DIFF_MIN && Math.abs(diff) <= PTS_DIFF_MAX) {
          this.validPtsDiffs += 1;
          const measured = diff > 0 ? Math.round(90000 / diff) : 0;
          if (measured >= FPS_MIN && measured <= FPS_MAX) {
            // 设备按解码序推流时（显示重排在渲染层做），到达序 PTS 差值正负交错，
            // 单个差值测得的"帧率"是噪声（实测 10/8fps 来回跳）——只有连续一致
            // 的差值才把帧率交给 PTS，否则始终由到达节奏 EWMA 供数。
            this.fpsStreak =
              this.lastMeasuredFps !== null && Math.abs(measured - this.lastMeasuredFps) <= 1
                ? this.fpsStreak + 1
                : 1;
            this.lastMeasuredFps = measured;
            // Reordered presentation timestamps do not describe decode-order frame intervals.
            if (this.ptsBacktracks < 2 && this.fpsStreak >= PTS_FPS_STREAK) {
              this.detectedFps = measured;
              this.fpsFromPts = true;
            }
          }
          if (diff < 0) {
            this.fpsStreak = 0;
            this.lastMeasuredFps = null;
          }
          if (this.validPtsDiffs >= PTS_TRUST_THRESHOLD) this.ptsTrusted = true;
        } else {
          if (!this.ptsTrusted) this.validPtsDiffs = 0;
          // 一致性序列被任何窗口外差值打断（乱序流的负差值 / 网络空洞）：
          // 否则乱序流中重复出现的同值差值（如两次 +9000）会被误判为连续一致
          this.fpsStreak = 0;
          this.lastMeasuredFps = null;
        }
        // 已采信的流偶发抖动（网络空洞）不降级——只有 reset()（重连/SPS 变更）清除信任
      }
      this.lastPts = pts90k;
    }

    const arrivalFps = windowFps ?? this.ewmaFps;
    if (!this.fpsFromPts && arrivalFps !== null) {
      const clamped = Math.min(FPS_MAX, Math.max(FPS_MIN, Math.round(arrivalFps)));
      if (clamped !== this.detectedFps) this.detectedFps = clamped;
    }

    const dts = this.nextDts;
    this.nextDts += Math.round(90000 / this.detectedFps);

    if (this.ptsTrusted && pts90k !== undefined && this.firstPts !== undefined) {
      // uint32 减法（|0 强制 int32）天然支持 90kHz 时间戳回绕
      const ptsRelative = (pts90k - this.firstPts) | 0;
      return { dts, ptsRelative, cto: ptsRelative - dts, fps: this.detectedFps };
    }
    // 合成：PTS 不可信时展示时间与解码时间同轨（cto=0），由到达节奏决定播放速率
    return { dts, ptsRelative: dts, cto: 0, fps: this.detectedFps };
  }
}
