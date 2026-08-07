/**
 * Mic capture AudioWorklet Processor
 *
 * Capture-direction counterpart of pcm-processor.js. Runs on the dedicated
 * audio thread and turns the live microphone input into fixed-size S16LE,
 * 48 kHz, mono chunks suitable for streaming over the talk WebSocket.
 *
 *  - Mono down-mix (averages all input channels)
 *  - Float32 → S16LE conversion (little-endian)
 *  - Fixed 1024-sample chunks (~21 ms @ 48 kHz): low latency without a
 *    WebSocket frame per 128-sample quantum
 *  - Per-chunk RMS level for a UI meter
 *
 * Each ready chunk is posted as { buffer: ArrayBuffer, level: number }; the
 * ArrayBuffer is transferred (zero-copy) to the main thread.
 */

const CHUNK_SIZE = 1024; // ~21 ms at 48 kHz

class MicProcessor extends AudioWorkletProcessor {
  constructor() {
    super();
    this.accum = new Float32Array(CHUNK_SIZE);
    this.pos = 0;
  }

  process(inputs) {
    const input = inputs[0];
    if (!input || input.length === 0) return true;
    const ch0 = input[0];
    if (!ch0 || ch0.length === 0) return true;

    const nch = input.length;
    const frame = ch0.length; // typically 128

    for (let i = 0; i < frame; i++) {
      // Mono down-mix
      let sum = 0;
      for (let c = 0; c < nch; c++) {
        sum += input[c][i];
      }
      this.accum[this.pos++] = sum / nch;

      if (this.pos >= this.accum.length) {
        this.flush();
      }
    }
    return true;
  }

  flush() {
    const len = this.pos;
    this.pos = 0;
    if (len === 0) return;

    // RMS level over the chunk for the UI meter
    let sumSq = 0;
    for (let i = 0; i < len; i++) {
      const s = this.accum[i];
      sumSq += s * s;
    }
    const level = Math.sqrt(sumSq / len);

    const buf = new ArrayBuffer(len * 2);
    const view = new DataView(buf);
    for (let i = 0; i < len; i++) {
      let s = this.accum[i];
      if (s > 1) s = 1;
      else if (s < -1) s = -1;
      view.setInt16(i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true); // little-endian
    }

    this.port.postMessage({ buffer: buf, level }, [buf]);
  }
}

registerProcessor('mic-processor', MicProcessor);
