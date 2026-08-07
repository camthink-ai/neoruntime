/**
 * PCM AudioWorklet Processor
 *
 * Runs on the dedicated audio thread — immune to main-thread blocking from
 * React renders, GC, or WebSocket processing.
 *
 * Features:
 *  - Ring buffer with overflow protection
 *  - Soft noise gate (RMS lookahead + smooth attack/release)
 *  - Clean underrun handling (no partial fill -> no clicks)
 */

const RING_SECONDS = 3;

class PcmProcessor extends AudioWorkletProcessor {
  constructor() {
    super();

    this.ring = new Float32Array(sampleRate * RING_SECONDS);
    this.rPos = 0;
    this.wPos = 0;
    this.started = false;
    this.syncDelay = Math.round(0.5 * sampleRate);

    // Noise-gate state
    this.gateGain = 0;
    // Lowered from 0.008 (~-42 dB) to 0.003 (~-50 dB): the analog Capture
    // Boost (+20 dB) was the hiss source and is now off, so the noise floor
    // dropped ~20 dB. The old threshold gated out the now-legitimate quieter
    // speech ("开启喇叭PC听不到声音"). A lower threshold is safe because the
    // hiss it existed to mute is gone.
    this.gateThreshold = 0.003;
    this.attackRate = 1 / (sampleRate * 0.005); // 5 ms open
    this.releaseRate = 1 / (sampleRate * 0.15); // 150 ms close

    this.port.onmessage = e => {
      const d = e.data;
      if (d.type === 'pcm') {
        this.enqueue(d.buffer);
      } else if (d.type === 'config') {
        if (d.syncDelayMs !== undefined) {
          this.syncDelay = Math.round((d.syncDelayMs / 1000) * sampleRate);
        }
        if (d.noiseGateThreshold !== undefined) {
          this.gateThreshold = d.noiseGateThreshold;
        }
      }
    };
  }

  enqueue(ab) {
    const pcm = new Int16Array(ab);
    const len = pcm.length;
    const cap = this.ring.length;

    // Drop oldest data if write would overflow
    const avail = this._avail();
    if (len > cap - avail) {
      this.rPos = (this.rPos + (len - (cap - avail))) % cap;
    }

    for (let i = 0; i < len; i++) {
      this.ring[this.wPos] = pcm[i] / 32768;
      this.wPos = (this.wPos + 1) % cap;
    }

    if (!this.started && this._avail() >= this.syncDelay) {
      this.started = true;
      this.port.postMessage({ type: 'playing' });
    }
  }

  _avail() {
    const w = this.wPos;
    const r = this.rPos;
    return w >= r ? w - r : this.ring.length - r + w;
  }

  process(_, outputs) {
    const ch = outputs[0][0];
    if (!ch) return true;

    // Not enough data yet (pre-buffer or underrun)
    if (!this.started || this._avail() < ch.length) {
      for (let i = 0; i < ch.length; i++) {
        this.gateGain = Math.max(0, this.gateGain - this.releaseRate * 4);
        ch[i] = 0;
      }
      return true;
    }

    // Lookahead RMS for noise gate
    let sumSq = 0;
    for (let i = 0; i < ch.length; i++) {
      const s = this.ring[(this.rPos + i) % this.ring.length];
      sumSq += s * s;
    }
    const rms = Math.sqrt(sumSq / ch.length);
    const target = rms > this.gateThreshold ? 1 : 0;

    for (let i = 0; i < ch.length; i++) {
      const raw = this.ring[this.rPos];

      // Smooth gate ramp
      if (this.gateGain < target) {
        this.gateGain = Math.min(target, this.gateGain + this.attackRate);
      } else {
        this.gateGain = Math.max(target, this.gateGain - this.releaseRate);
      }

      ch[i] = raw * this.gateGain;
      this.rPos = (this.rPos + 1) % this.ring.length;
    }

    return true;
  }
}

registerProcessor('pcm-processor', PcmProcessor);
