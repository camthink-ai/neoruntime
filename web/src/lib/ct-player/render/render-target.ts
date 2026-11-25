/**
 * RenderTargetManager：video + canvas 双元素共存（技术切换不黑屏，底座项目验证的做法）。
 * MSE 管道显示 <video>；WebCodecs/WASM 管道显示 <canvas>。
 */

import { createLogger } from '../shared/log';

const log = createLogger('render');

export type SurfaceKind = 'video' | 'canvas';

/** Picture rectangle inside the canvas backing store, in device pixels. */
export interface ContentRect {
  x: number;
  y: number;
  w: number;
  h: number;
}

export interface RenderTargetOptions {
  /** 'contain'（默认）| 'cover' */
  objectFit?: 'contain' | 'cover';
}

export class RenderTargetManager {
  private container: HTMLElement | null = null;
  private video: HTMLVideoElement | null = null;
  private canvas: HTMLCanvasElement | null = null;
  private canvasContext: '2d' | 'webgl' | null = null;
  private activeSurface: SurfaceKind | null = null;
  private objectFit: 'contain' | 'cover';
  /** 记录我们创建的元素以便销毁时移除 */
  private ownedElements: HTMLElement[] = [];
  private externalVideo: HTMLVideoElement | null = null;
  private classObserver: MutationObserver | null = null;
  /** Latest picture rectangle reported by the active canvas pipeline. */
  private contentRect: ContentRect | null = null;

  constructor(options: RenderTargetOptions = {}) {
    this.objectFit = options.objectFit ?? 'contain';
  }

  attach(target: HTMLElement | string): this {
    const el =
      typeof target === 'string'
        ? document.querySelector<HTMLElement>(target)
        : target;
    if (!el) {
      throw new Error(`[player] attach target not found: ${String(target)}`);
    }
    if (this.container === el) return this;
    this.destroyElements();
    this.externalVideo = el instanceof HTMLVideoElement ? el : null;
    this.container = this.externalVideo ? this.externalVideo.parentElement : el;
    if (!this.container) throw new Error('[player] video requires a parent element');
    if (this.externalVideo) return this;
    if (getComputedStyle(el).position === 'static') {
      el.style.position = 'relative';
    }
    return this;
  }

  private ensureElements(): void {
    if (!this.container || this.video) return;
    const video = this.externalVideo ?? document.createElement('video');
    video.muted = true;
    video.playsInline = true;
    video.autoplay = true;
    video.setAttribute('playsinline', '');
    video.setAttribute('webkit-playsinline', '');
    if (!this.externalVideo) video.style.cssText =
      'position:absolute;inset:0;width:100%;height:100%;display:none;background:#000;object-fit:' +
      this.objectFit +
      ';';
    const canvas = document.createElement('canvas');
    canvas.style.cssText =
      'position:absolute;inset:0;width:100%;height:100%;display:none;background:#000;';
    if (this.externalVideo) {
      canvas.width = 0;
      canvas.height = 0;
      canvas.className = video.className;
      canvas.setAttribute('data-player-canvas', '');
      this.classObserver = new MutationObserver(() => { if (this.canvas) this.canvas.className = video.className; });
      this.classObserver.observe(video, { attributes: true, attributeFilter: ['class'] });
    } else this.container.prepend(video);
    // Insert engine elements at the head / right after the external video so
    // host-rendered overlays (stats panel, OSd layers) that already sit in the
    // container keep painting above the opaque canvas.
    video.after(canvas);
    this.video = video;
    this.canvas = canvas;
    this.ownedElements = this.externalVideo ? [canvas] : [video, canvas];
  }

  getVideo(): HTMLVideoElement | null {
    this.ensureElements();
    return this.video;
  }

  /** 运行时切换填充模式；自建 video 更新内联样式，外部 video 由宿主样式控制。 */
  setObjectFit(fit: 'contain' | 'cover'): void {
    this.objectFit = fit;
    if (this.video && !this.externalVideo) this.video.style.objectFit = fit;
  }

  getCanvas(context?: '2d' | 'webgl'): HTMLCanvasElement | null {
    this.ensureElements();
    if (context && this.canvas && this.canvasContext && this.canvasContext !== context) {
      const replacement = document.createElement('canvas');
      replacement.style.cssText = this.canvas.style.cssText;
      replacement.className = this.canvas.className;
      if (this.externalVideo) replacement.setAttribute('data-player-canvas', '');
      this.canvas.replaceWith(replacement);
      this.ownedElements = this.ownedElements.map((element) => element === this.canvas ? replacement : element);
      this.canvas = replacement;
    }
    if (context) this.canvasContext = context;
    return this.canvas;
  }

  /** 切换显示面（管道初始化时调用）。 */
  useSurface(kind: SurfaceKind | null): void {
    this.ensureElements();
    if (kind === this.activeSurface) return;
    this.activeSurface = kind;
    if (this.video) this.video.style.display = kind === 'video' ? '' : 'none';
    if (this.canvas) this.canvas.style.display = kind === 'canvas' ? '' : 'none';
    log.debug(`surface -> ${String(kind)}`);
  }

  /** Picture rectangle within the canvas (device pixels); null fills the canvas. */
  setContentRect(rect: ContentRect | null): void {
    this.contentRect = rect;
  }

  /** 截图（video 面走临时 canvas 抓帧；canvas 面按内容矩形裁剪后 toBlob）。 */
  async snapshot(): Promise<Blob> {
    if (this.activeSurface === 'canvas' && this.canvas && this.canvas.width > 0) {
      const canvas = this.canvas;
      // Canvas pipelines draw the letterboxed picture into a container-sized
      // backing store; crop to the content rectangle so snapshots carry only
      // the picture (cover rects are clamped to the canvas bounds).
      const rect = this.contentRect;
      const x = Math.max(0, Math.floor(rect?.x ?? 0));
      const y = Math.max(0, Math.floor(rect?.y ?? 0));
      const w = Math.min(canvas.width - x, Math.ceil(rect ? rect.w : canvas.width));
      const h = Math.min(canvas.height - y, Math.ceil(rect ? rect.h : canvas.height));
      if (w >= 1 && h >= 1 && (x > 0 || y > 0 || w < canvas.width || h < canvas.height)) {
        const c = document.createElement('canvas');
        c.width = w;
        c.height = h;
        c.getContext('2d')?.drawImage(canvas, x, y, w, h, 0, 0, w, h);
        return canvasToPng(c);
      }
      return canvasToPng(canvas);
    }
    const video = this.video;
    if (!video || video.videoWidth === 0) {
      throw new Error('no rendered frame available');
    }
    const c = document.createElement('canvas');
    // 注意宏块对齐：编码分辨率可能是 1088，videoWidth 已是裁剪后的显示尺寸
    c.width = video.videoWidth;
    c.height = video.videoHeight;
    c.getContext('2d')?.drawImage(video, 0, 0, c.width, c.height);
    return canvasToPng(c);
  }

  private destroyElements(): void {
    this.classObserver?.disconnect();
    this.classObserver = null;
    this.contentRect = null;
    for (const el of this.ownedElements) {
      el.remove();
    }
    this.ownedElements = [];
    this.video = null;
    this.canvas = null;
    this.canvasContext = null;
    this.activeSurface = null;
    if (this.externalVideo) this.externalVideo.style.display = '';
    this.externalVideo = null;
  }

  destroy(): void {
    this.destroyElements();
    this.container = null;
  }
}

function canvasToPng(c: HTMLCanvasElement): Promise<Blob> {
  return new Promise<Blob>((resolve, reject) => {
    c.toBlob((b) => (b ? resolve(b) : reject(new Error('toBlob failed'))), 'image/png');
  });
}
