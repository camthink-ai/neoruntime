import { describe, expect, it } from 'vitest';
import { resolveFrozenFrameSource } from './freezeFrame';

function setVideoDimensions(
  video: HTMLVideoElement,
  width: number,
  height: number
) {
  Object.defineProperties(video, {
    videoWidth: { configurable: true, value: width },
    videoHeight: { configurable: true, value: height },
  });
}

describe('resolveFrozenFrameSource', () => {
  it('captures the rendered WebCodecs canvas instead of the hidden video', () => {
    const parent = document.createElement('div');
    const video = document.createElement('video');
    const renderer = document.createElement('canvas');
    renderer.dataset.aipcVideoRenderer = 'webcodecs';
    renderer.dataset.aipcFrameReady = 'true';
    renderer.width = 1280;
    renderer.height = 720;
    setVideoDimensions(video, 0, 0);
    parent.append(video, renderer);

    expect(resolveFrozenFrameSource(video)).toEqual({
      source: renderer,
      width: 1280,
      height: 720,
    });
  });

  it('ignores a WebCodecs canvas before its first rendered frame', () => {
    const parent = document.createElement('div');
    const video = document.createElement('video');
    const renderer = document.createElement('canvas');
    renderer.dataset.aipcVideoRenderer = 'webcodecs';
    renderer.dataset.aipcFrameReady = 'false';
    setVideoDimensions(video, 640, 360);
    parent.append(video, renderer);

    expect(resolveFrozenFrameSource(video)).toEqual({
      source: video,
      width: 640,
      height: 360,
    });
  });

  it('returns null when neither renderer has produced a frame', () => {
    const video = document.createElement('video');
    setVideoDimensions(video, 0, 0);
    expect(resolveFrozenFrameSource(video)).toBeNull();
  });
});
