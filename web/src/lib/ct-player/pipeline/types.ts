import type { EncodedFrame, PlayMode, TechKind } from '../shared/types';
import type { CTErrorCode } from '../shared/errors';
import type { LatencyParams } from '../core/presets';
import type { RenderTargetManager } from '../render/render-target';
import type { WasmKernelOptions } from '../wasm/kernel-client';

export interface PipelineStats {
  framesIn: number;
  framesOut: number;
  dropped: number;
  /** Decoded display size of the most recent rendered frame (canvas pipelines). */
  videoSize?: { width: number; height: number };
  drawTiming?: { currentGapMs: number | null; p95GapMs: number | null; stalls: number };
  decodeErrors?: number;
  avSync?: { status: 'waiting' | 'active' | 'unavailable'; driftMs: number | null; dropped: number; queued: number };
  wasmDrops?: { beforeConfiguration: number; waitingForKeyframe: number; queueOverflow: number; suspended: number; presentation?: number };
  wasmPresentation?: { queued: number; nextInMs: number | null; resets: number };
  wasmPerf?: { decodeMs: number | null; roundTripMs: number | null; renderMs: number | null; queue: number; peakQueue: number; samples: number; initMs: number | null; recentDecodeMs?: number | null; recentDecodeP95Ms?: number | null; threads?: number | null };
}

export interface PipelineTimeline {
  durationMs: number | null;
  positionMs: number;
  /** 已缓冲区间 [startSec, endSec] */
  ranges: Array<[number, number]>;
}

export interface PipelineContext {
  render: RenderTargetManager;
  latency: LatencyParams;
  objectFit: 'contain' | 'cover';
  mode: PlayMode;
  /** WASM 内核配置（仅 wasm 管道使用） */
  wasm?: WasmKernelOptions;
  audioClock?: { getPts90k: () => number | null; maxDriftMs: number };
  /** 致命错误 → FallbackMachine 决策 */
  onFatal: (code: CTErrorCode, detail?: unknown) => void;
  /** 首帧成功呈现 */
  onReady: () => void;
  /** 可恢复解码错误（供 HealthMonitor 风暴计数） */
  onDecodeError?: () => void;
}

export interface Pipeline {
  readonly tech: TechKind;
  init(): void;
  submit(frame: EncodedFrame): void;
  /** 页面后台/宿主暂停 */
  suspend(): void;
  resume(): void;
  stats(): PipelineStats;
  /** 实时延迟信息（MSE 可得，Canvas 路径为 null） */
  latencyInfo(): { latencyMs: number | null; bufferedMs: number | null };
  /** 回放控制（仅 playback 模式的管道实现） */
  seek?(sec: number): void;
  setSpeed?(rate: number): void;
  timeline?(): PipelineTimeline;
  /** 运行时更新渲染填充模式（下一帧生效） */
  setObjectFit?(fit: 'contain' | 'cover'): void;
  destroy(): void;
}
