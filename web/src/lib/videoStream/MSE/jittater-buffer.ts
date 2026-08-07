// Web/src/lib/MSE/jittater-buffer.ts

interface BufferedFrame {
  data: Uint8Array;
  timestamp: number;
  receivedAt: number; // Receive timestamp, used to calculate delay
}

export class JitterBuffer {
  private buffer: BufferedFrame[] = [];

  private readonly frameInterval: number = 40; // ms (25fps)

  private readonly minBufferSize: number = 10; // Increase to 10 frames (400ms) to reduce playback stops

  private readonly maxBufferSize: number = 50; // Increase to 50 frames (2s) to prevent delay accumulation

  private readonly lowWaterMark: number = 5; // Low water mark, pause when below this value

  private sequence: number = 0;

  private isPlaying: boolean = false;

  initialize(): void {
    this.buffer = [];
    this.sequence = 0;
    this.isPlaying = false;
  }

  addFrame(data: Uint8Array): void {
    if (data.length === 0) return;

    const receivedAt = Date.now();
    const timestamp = this.sequence * this.frameInterval;
    this.buffer.push({ data, timestamp, receivedAt });
    this.sequence++;

    // Smart frame dropping: when buffer is too large, prioritize keeping newer frames
    if (this.buffer.length > this.maxBufferSize) {
      // Calculate delay for each frame, prioritize discarding old frames with excessive delay
      const now = Date.now();
      const maxDelay = 1000; // Maximum allowed delay 1 second

      // Traverse from old to new, find first frame with acceptable delay
      while (this.buffer.length > this.maxBufferSize * 0.8) {
        const oldestFrame = this.buffer[0];
        const delay = now - oldestFrame.receivedAt;
        if (delay > maxDelay) {
          this.buffer.shift();
        } else {
          break;
        }
      }

      // If still need to discard, discard oldest frames
      while (this.buffer.length > this.maxBufferSize) {
        this.buffer.shift();
      }
    }

    // Start playback after reaching minimum buffer
    if (!this.isPlaying && this.buffer.length >= this.minBufferSize) {
      this.isPlaying = true;
    }
  }

  getNextFrame(): BufferedFrame | null {
    if (!this.isPlaying || this.buffer.length === 0) {
      return null;
    }

    const frame = this.buffer.shift();

    // Pause when buffer is below low water mark, wait for more frames
    if (this.buffer.length < this.lowWaterMark) {
      this.isPlaying = false;
    }

    return frame || null;
  }

  getBufferSize(): number {
    return this.buffer.length;
  }

  clear(): void {
    this.buffer = [];
    this.sequence = 0;
    this.isPlaying = false;
  }
}
