// @ts-nocheck
/**
 * Reconnect policy: normal closes (1000–1005) do not reconnect by default;
 * superseded connections are reported without retrying. Other failures use
 * exponential backoff with jitter and a bounded retry window.
 */

import type { ReconnectPolicy } from '../shared/types';

export const DEFAULT_RECONNECT_POLICY: ReconnectPolicy = {
  baseDelayMs: 1000,
  maxDelayMs: 10000,
  jitterMs: 300,
  maxAttempts: 10,
  windowMs: 5 * 60 * 1000,
  supersededCodes: [1008],
  supersededReasons: ['Connection replaced'],
  reconnectOnNormalClose: false,
};

export type ReconnectDecision =
  | { action: 'reconnect'; delayMs: number; attempt: number }
  | { action: 'report' }
  | { action: 'superseded' }
  | { action: 'give-up' };

export class ReconnectController {
  private attemptCount = 0;
  private firstRetryAt: number | null = null;
  private manual = false;

  constructor(
    private readonly policy: ReconnectPolicy = DEFAULT_RECONNECT_POLICY,
    private readonly now: () => number = () => Date.now(),
    private readonly random: () => number = Math.random
  ) {}

  get attempts(): number {
    return this.attemptCount;
  }

  /** 宿主主动关闭：后续 onClose 一律上报不重连。 */
  markManual(): void {
    this.manual = true;
  }

  onOpen(): void {
    this.attemptCount = 0;
    this.firstRetryAt = null;
    this.manual = false;
  }

  onClose(code: number, reason: string): ReconnectDecision {
    if (this.manual) return { action: 'report' };
    if (
      this.policy.supersededCodes.includes(code) ||
      (reason !== '' && this.policy.supersededReasons.includes(reason))
    ) {
      return { action: 'superseded' };
    }
    if (code >= 1000 && code <= 1005 && !this.policy.reconnectOnNormalClose) {
      return { action: 'report' };
    }

    const now = this.now();
    if (this.firstRetryAt === null) this.firstRetryAt = now;
    // 窗口滑过：计数归零重新累积
    if (now - this.firstRetryAt >= this.policy.windowMs) {
      this.attemptCount = 0;
      this.firstRetryAt = now;
    }
    if (this.attemptCount >= this.policy.maxAttempts) {
      return { action: 'give-up' };
    }
    this.attemptCount += 1;
    const expo = Math.min(
      this.policy.baseDelayMs * 2 ** (this.attemptCount - 1),
      this.policy.maxDelayMs
    );
    const jitter = Math.floor(this.random() * this.policy.jitterMs);
    return { action: 'reconnect', delayMs: expo + jitter, attempt: this.attemptCount };
  }
}

export function mergePolicy(overrides?: Partial<ReconnectPolicy>): ReconnectPolicy {
  return { ...DEFAULT_RECONNECT_POLICY, ...overrides };
}
