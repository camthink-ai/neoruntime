export type FrozenFrameSource = {
  source: HTMLCanvasElement | HTMLVideoElement;
  width: number;
  height: number;
};

const WEB_CODECS_RENDERER_SELECTOR = 'canvas[data-aipc-video-renderer="webcodecs"][data-aipc-frame-ready="true"]';

/**
 * Resolve the element that actually owns the last rendered frame.
 * WebCodecs paints into a sibling canvas and deliberately hides <video>,
 * while the MSE fallback paints into <video> itself.
 */
export function resolveFrozenFrameSource(
  video: HTMLVideoElement
): FrozenFrameSource | null {
  const renderer = video.parentElement?.querySelector<HTMLCanvasElement>(
    WEB_CODECS_RENDERER_SELECTOR
  );
  if (renderer && renderer.width > 0 && renderer.height > 0) {
    return {
      source: renderer,
      width: renderer.width,
      height: renderer.height,
    };
  }

  if (video.videoWidth <= 0 || video.videoHeight <= 0) return null;
  return {
    source: video,
    width: video.videoWidth,
    height: video.videoHeight,
  };
}
