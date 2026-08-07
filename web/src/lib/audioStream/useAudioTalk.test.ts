import { act, renderHook, waitFor } from '@testing-library/react';
import { beforeEach, describe, expect, it, vi } from 'vitest';
import { fetchAudioStatus } from '@/services/settings';
import { toast } from 'sonner';
import { useAudioControlStore } from '@/store/audio';
import { useAudioTalk } from './useAudioTalk';

// `vi.hoisted` runs before vi.mock factories so the mock fn + mutable active
// flag are defined at hoist time and can be referenced from the factory.
const mocks = vi.hoisted(() => {
  const activeRef = { active: false };
  return {
    activeRef,
    mockStart: vi.fn(async () => {
      activeRef.active = true;
    }),
    mockStop: vi.fn(async () => {
      activeRef.active = false;
    }),
  };
});

// A real class (not an arrow fn) so `new AudioTalker(...)` constructs cleanly;
// start/stop delegate to the shared mock fns, `active` reads the shared flag.
vi.mock('./audioTalker', () => ({
  AudioTalker: class {
    start = mocks.mockStart;

    stop = mocks.mockStop;

    // Proxies the shared mock flag rather than instance state.
    // eslint-disable-next-line class-methods-use-this
    get active() {
      return mocks.activeRef.active;
    }
  },
}));

vi.mock('@/services/settings', () => ({
  fetchAudioStatus: vi.fn(),
  audioTalkStreamUrl: () => 'ws://test/audio/talk',
}));

vi.mock('react-i18next', () => {
  const t = (_k: string, fb?: string) => fb ?? _k;
  return { useTranslation: () => ({ t }) };
});

vi.mock('sonner', () => ({ toast: { error: vi.fn() } }));

const resetStore = () => {
  useAudioControlStore.setState({
    listenEnabled: false,
    muted: true,
    volume: 1.0,
    captureAvailable: false,
    playbackEnabled: false,
  });
};

describe('useAudioTalk — toggleTalk', () => {
  beforeEach(() => {
    resetStore();
    mocks.activeRef.active = false;
    mocks.mockStart.mockReset();
    mocks.mockStop.mockReset();
    // Re-implement (not mockResolvedValue) so start()/stop() also flip the
    // shared `active` flag — startTalk gates setTalking(true) on
    // talkerRef.current.active being true after start() resolves.
    mocks.mockStart.mockImplementation(async () => {
      mocks.activeRef.active = true;
    });
    mocks.mockStop.mockImplementation(async () => {
      mocks.activeRef.active = false;
    });
    vi.mocked(fetchAudioStatus).mockResolvedValue({
      capturing: true,
      playback_enabled: true,
    } as never);
  });

  it('shows a localized, actionable error when no microphone is found', async () => {
    mocks.mockStart.mockRejectedValueOnce(
      new DOMException('Requested device not found', 'NotFoundError')
    );
    const { result } = renderHook(() => useAudioTalk({ enabled: true }));
    await waitFor(() => {
      expect(useAudioControlStore.getState().playbackEnabled).toBe(true);
    });

    act(() => {
      result.current.toggleTalk();
    });

    await waitFor(() => {
      expect(toast.error).toHaveBeenCalledWith(
        'No microphone was detected. Connect one and try again.'
      );
    });
    expect(result.current.talking).toBe(false);
  });

  it('rejects the toggle when the playback gate is off', async () => {
    // Arrange — device speaker output is off.
    vi.mocked(fetchAudioStatus).mockResolvedValue({
      capturing: true,
      playback_enabled: false,
    } as never);

    // Act
    const { result } = renderHook(() => useAudioTalk({ enabled: true }));
    await waitFor(() => {
      expect(useAudioControlStore.getState().playbackEnabled).toBe(false);
    });
    act(() => {
      result.current.toggleTalk();
    });

    // Assert — start is never called; the toggle is a no-op.
    await waitFor(() => {
      expect(mocks.mockStart).not.toHaveBeenCalled();
    });
    expect(result.current.talking).toBe(false);
  });

  it('starts talk on first toggle and stops on the next', async () => {
    // Arrange
    const { result } = renderHook(() => useAudioTalk({ enabled: true }));
    await waitFor(() => {
      expect(useAudioControlStore.getState().playbackEnabled).toBe(true);
    });

    // Act — first click starts.
    await act(async () => {
      result.current.toggleTalk();
    });

    // Assert — talking went live and start was called once.
    await waitFor(() => {
      expect(result.current.talking).toBe(true);
    });
    expect(mocks.mockStart).toHaveBeenCalledTimes(1);

    // Act — second click stops.
    await act(async () => {
      result.current.toggleTalk();
    });

    // Assert
    await waitFor(() => {
      expect(result.current.talking).toBe(false);
    });
    expect(mocks.mockStop).toHaveBeenCalled();
  });

  it('guards against double-click during the async start window', async () => {
    // Arrange — make start() pending so the second click lands while the first
    // is still in flight.
    let resolveStart: () => void = () => {};
    mocks.mockStart.mockImplementation(
      () => new Promise<void>(resolve => {
          resolveStart = resolve;
        })
    );
    const { result } = renderHook(() => useAudioTalk({ enabled: true }));
    await waitFor(() => {
      expect(useAudioControlStore.getState().playbackEnabled).toBe(true);
    });

    // Act — two rapid clicks synchronously.
    act(() => {
      result.current.toggleTalk(); // first: enters the async start window
      result.current.toggleTalk(); // second: startingRef is true → finishTalk
    });

    // Assert — start is called exactly once (the guard prevented a second
    // session); the second click routed through stop instead.
    expect(mocks.mockStart).toHaveBeenCalledTimes(1);
    expect(mocks.mockStop).toHaveBeenCalled();

    // Cleanup — let the pending start settle so no dangling microtask remains.
    resolveStart();
  });
});
