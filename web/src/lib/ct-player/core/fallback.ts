/**
 * FallbackMachine：技术×码流×清晰度 三维降级状态机（P2：决策集中、纯逻辑可单测）。
 * Player 执行其输出的 Action；每次降级产出 FallbackTransition 供事件上报。
 */

import type {
  FallbackPolicy,
  TechKind,
  VideoCodec,
} from '../shared/types';
import type { CapabilityProfile } from '../shared/capability';
import type { CTErrorCode, CTError } from '../shared/errors';
import { ctError } from '../shared/errors';
import type { FallbackLevel } from '../shared/events';

export type FsmStateName =
  | 'idle'
  | 'switching'
  | 'running'
  | 'restarting'
  | 'waiting-stream'
  | 'terminal';

export type FsmInput =
  | { type: 'start'; codec: VideoCodec; capability: CapabilityProfile; policy: FallbackPolicy }
  | { type: 'ready' }
  | { type: 'fatal'; code: CTErrorCode }
  | { type: 'codec-mismatch' }
  | { type: 'stream-arrived'; codec: VideoCodec }
  | { type: 'stream-declined' }
  | { type: 'stall' };

export type FsmAction =
  | { type: 'use-tech'; tech: TechKind }
  | { type: 'restart-tech'; tech: TechKind }
  | { type: 'request-stream'; codec: VideoCodec }
  | { type: 'terminal'; error: CTError };

export interface FallbackTransition {
  from: TechKind | null;
  to: TechKind | 'stream-switch' | 'none';
  level: FallbackLevel;
  reason: string;
}

export interface FsmOutput {
  actions: FsmAction[];
  transition?: FallbackTransition;
}

/** 按能力过滤生成技术链（policy.techChain 可覆盖顺序）。 */
export function selectTechChain(
  codec: VideoCodec,
  capability: CapabilityProfile,
  policy: FallbackPolicy
): TechKind[] {
  // wasm 为最终兜底：硬解链（按 codec 能力过滤）在前
  const preferred: TechKind[] = ['webcodecs', 'mse', 'wasm'];
  const ordered = policy.techChain ? policy.techChain : preferred;
  const available = ordered.filter((tech) => {
    if (tech === 'webcodecs') {
      return codec === 'h264' ? capability.webcodecsH264 : capability.webcodecsH265;
    }
    if (tech === 'mse') {
      return codec === 'h264' ? capability.mseH264 : capability.mseH265;
    }
    if (tech === 'wasm') {
      // 显式指定 techChain 视为专家模式：只要求 WebAssembly 存在（用于测试/强制软解）。
      const explicit = policy.techChain !== undefined;
      return capability.wasm && (policy.allowQualityFallback || explicit);
    }
    return false; // whep 由专用 source 接入，不进降级链
  });
  return available;
}

export class FallbackMachine {
  state: FsmStateName = 'idle';
  techChain: TechKind[] = [];
  techIndex = -1;
  codec: VideoCodec | null = null;
  private capability: CapabilityProfile | null = null;
  private policy: FallbackPolicy | null = null;
  private restarts = 0;
  /** 码流级降级只请求一次：declined 后不得重复请求（否则与宿主死循环） */
  private streamFallbackTried = false;
  private streamFallbackCause: CTErrorCode = 'CODEC_MISMATCH';

  get currentTech(): TechKind | null {
    return this.techIndex >= 0 ? this.techChain[this.techIndex] ?? null : null;
  }

  dispatch(input: FsmInput): FsmOutput {
    switch (input.type) {
      case 'start':
        return this.onStart(input);
      case 'ready':
        return this.onReady();
      case 'fatal':
        return this.onFatal(input.code, input);
      case 'codec-mismatch':
        return this.onCodecMismatch();
      case 'stream-arrived':
        return this.onStreamArrived(input.codec);
      case 'stream-declined':
        return this.onStreamDeclined();
      case 'stall':
        return this.onStall();
    }
  }

  private onStart(input: {
    codec: VideoCodec;
    capability: CapabilityProfile;
    policy: FallbackPolicy;
  }): FsmOutput {
    if (this.state !== 'idle' && this.state !== 'terminal') {
      return { actions: [] };
    }
    this.codec = input.codec;
    this.capability = input.capability;
    this.policy = input.policy;
    this.restarts = 0;
    this.techChain = selectTechChain(input.codec, input.capability, input.policy);
    if (this.techChain.length === 0) {
      this.state = 'terminal';
      return {
        actions: [
          {
            type: 'terminal',
            error: ctError('CODEC_UNSUPPORTED', {
              detail: { codec: input.codec, platform: input.capability.platform },
              recovery: 'no tech in fallback chain can decode this codec',
            }),
          },
        ],
      };
    }
    this.techIndex = 0;
    this.state = 'switching';
    return {
      actions: [{ type: 'use-tech', tech: this.techChain[0] }],
      transition: {
        from: null,
        to: this.techChain[0],
        level: 'tech',
        reason: 'initial selection',
      },
    };
  }

  private onReady(): FsmOutput {
    if (this.state === 'switching' || this.state === 'restarting') {
      this.state = 'running';
      // 注意：restarts 不在此清零——连续看门狗重启的计数必须跨 ready 累积，
      // 否则每次短暂恢复都会重置额度，maxInternalRestarts 永远触不到。
    }
    return { actions: [] };
  }

  private onFatal(code: CTErrorCode, _input?: FsmInput): FsmOutput {
    if (this.state === 'idle' || this.state === 'terminal' || this.state === 'waiting-stream') {
      return { actions: [] };
    }
    const prevTech = this.currentTech;
    const next = this.techIndex + 1;
    if (next < this.techChain.length) {
      this.techIndex = next;
      this.state = 'switching';
      this.restarts = 0;
      return {
        actions: [{ type: 'use-tech', tech: this.techChain[next] }],
        transition: { from: prevTech, to: this.techChain[next], level: 'tech', reason: code },
      };
    }
    return this.degradeBeyondTechs(code, prevTech);
  }

  /** 技术链耗尽：h265 → 码流级（请求宿主切 h264，仅一次）；其后清晰度级/终止。 */
  private degradeBeyondTechs(code: CTErrorCode, prevTech: TechKind | null): FsmOutput {
    if (this.codec === 'h265' && this.policy?.allowStreamFallback && !this.streamFallbackTried) {
      this.streamFallbackTried = true;
      this.streamFallbackCause = code;
      this.state = 'waiting-stream';
      return {
        actions: [{ type: 'request-stream', codec: 'h264' }],
        transition: { from: prevTech, to: 'stream-switch', level: 'stream', reason: code },
      };
    }
    if (this.policy?.allowQualityFallback && this.capability?.wasm && !this.techChain.includes('wasm')) {
      // WASM 失败后再次触发 fatal 会进入终止状态。
      this.techChain = [...this.techChain, 'wasm'];
      this.techIndex = this.techChain.length - 1;
      this.state = 'switching';
      return {
        actions: [{ type: 'use-tech', tech: 'wasm' }],
        transition: { from: prevTech, to: 'wasm', level: 'quality', reason: code },
      };
    }
    this.state = 'terminal';
    return {
      actions: [{ type: 'terminal', error: ctError(code, { detail: { codec: this.codec } }) }],
    };
  }

  private onCodecMismatch(): FsmOutput {
    // 真实 SPS 探测失败：当前 codec 在硬解链上不可解。
    // wasm 仍在链中时先降 wasm（技术级），链尽才走码流级——与 onFatal 同路径。
    return this.onFatal('CODEC_MISMATCH');
  }

  private onStreamArrived(codec: VideoCodec): FsmOutput {
    if (this.state !== 'waiting-stream' && this.state !== 'running') {
      return { actions: [] };
    }
    this.codec = codec;
    this.techChain = selectTechChain(codec, this.capability!, this.policy!);
    this.restarts = 0;
    this.streamFallbackTried = false;
    if (this.techChain.length === 0) {
      this.state = 'terminal';
      return {
        actions: [
          { type: 'terminal', error: ctError('CODEC_UNSUPPORTED', { detail: { codec } }) },
        ],
      };
    }
    this.techIndex = 0;
    this.state = 'switching';
    return {
      actions: [{ type: 'use-tech', tech: this.techChain[0] }],
      transition: {
        from: null,
        to: this.techChain[0],
        level: 'stream',
        reason: 'stream switched after fallback request',
      },
    };
  }

  private onStreamDeclined(): FsmOutput {
    if (this.state !== 'waiting-stream') return { actions: [] };
    return this.degradeBeyondTechs(this.streamFallbackCause, this.currentTech);
  }

  private onStall(): FsmOutput {
    // Startup stall: the socket is open but no frame ever arrived, so no
    // pipeline exists to restart and the running-state recovery below never
    // engages — the page would spin forever. Report a terminal failure so
    // the host can surface its Reload/disconnected control (or reconnect on
    // its own). HealthMonitor un-latches as soon as the first frame lands.
    if (this.state === 'idle') {
      return {
        actions: [
          { type: 'terminal', error: ctError('NO_KEYFRAME_TIMEOUT', { detail: { phase: 'startup' } }) },
        ],
      };
    }
    if (this.state !== 'running' && this.state !== 'restarting') {
      return { actions: [] };
    }
    const tech = this.currentTech;
    const max = this.policy?.maxInternalRestarts ?? 3;
    if (this.restarts < max && tech) {
      this.restarts += 1;
      this.state = 'restarting';
      return { actions: [{ type: 'restart-tech', tech }] };
    }
    return this.onFatal('NO_KEYFRAME_TIMEOUT', { type: 'stall' });
  }
}
