/* eslint-disable class-methods-use-this */
import { afterEach, describe, expect, it, vi } from 'vitest';
import { VideoStreamPlayer } from './player';

type Handler = (payload: any) => void;
const mock = vi.hoisted(() => ({ handlers: new Map<string, Handler>() }));

vi.mock('@/lib/ct-player/core/player', () => ({
  CTPlayer: class {
    attach() { return this; }

    on(name: string, handler: (payload: any) => void) { mock.handlers.set(name, handler); }

    play() { return Promise.resolve(); }

    destroy() { /* noop mock */ }
  },
}));

describe('video loading state', () => {
  afterEach(() => { mock.handlers.clear(); });

  it('waits for playback readiness before reporting that the stream is working', async () => {
    const events: boolean[] = [];
    const listener = (event: Event) => events.push((event as CustomEvent<boolean>).detail);
    window.addEventListener('wv_work', listener);
    const player = new VideoStreamPlayer().initPlayer(document.createElement('video'));
    await player.start('ws://example.test');

    mock.handlers.get('stats')?.({ pps: 30, codec: 'h265', tech: 'wasm' });
    expect(events).toEqual([false]);
    mock.handlers.get('state')?.({ state: 'playing' });
    expect(events).toEqual([false, true]);

    player.waitForNextVideoData();
    mock.handlers.get('stats')?.({ pps: 30, codec: 'h265', tech: 'wasm' });
    expect(events).toEqual([false, true, false, true]);
    player.destroy();
    window.removeEventListener('wv_work', listener);
  });
});
