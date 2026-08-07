import { afterEach, describe, expect, it, vi } from 'vitest';
import { VideoStreamPlayer } from './player';

vi.mock('./MSE/msePlayer', () => ({
  default: class MockH264Player {
    handleVideoData = vi.fn();
  },
}));

type PlayerInternals = {
  webSocketWorker: Worker | null;
  setupWebSocketWorkerHandler: () => void;
};

const workEvents: boolean[] = [];
const handleWork = (event: Event) => {
  workEvents.push((event as CustomEvent<boolean>).detail);
};

afterEach(() => {
  window.removeEventListener('wv_work', handleWork);
  workEvents.length = 0;
});

describe('VideoStreamPlayer readiness', () => {
  it('becomes ready only after receiving non-empty video data', () => {
    window.addEventListener('wv_work', handleWork);

    const player = new VideoStreamPlayer();
    const worker = { onmessage: null } as unknown as Worker;
    const internals = player as unknown as PlayerInternals;
    internals.webSocketWorker = worker;
    internals.setupWebSocketWorkerHandler();

    worker.onmessage?.({ data: { type: 'open' } } as MessageEvent);
    expect(workEvents).toEqual([false]);

    worker.onmessage?.({
      data: { type: 'video-data', payload: new ArrayBuffer(1) },
    } as MessageEvent);
    expect(workEvents).toEqual([false, true]);

    worker.onmessage?.({
      data: { type: 'video-data', payload: new ArrayBuffer(1) },
    } as MessageEvent);
    expect(workEvents).toEqual([false, true]);

    player.waitForNextVideoData();
    expect(workEvents).toEqual([false, true, false]);

    worker.onmessage?.({
      data: { type: 'video-data', payload: new ArrayBuffer(1) },
    } as MessageEvent);
    expect(workEvents).toEqual([false, true, false, true]);
  });
});