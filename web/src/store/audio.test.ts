import { beforeEach, describe, expect, it } from 'vitest';
import { useAudioControlStore } from '@/store/audio';

// The store is a module singleton, so reset every slice to its initial value
// between tests to keep each case independent.
const reset = () => {
  useAudioControlStore.setState({
    listenEnabled: false,
    muted: true,
    volume: 1.0,
    captureAvailable: false,
    playbackEnabled: false,
  });
};

describe('useAudioControlStore', () => {
  beforeEach(reset);

  describe('toggleMonitor', () => {
    it('turns monitor on from the initial off state', () => {
      // Arrange — initial state: listenEnabled=false, muted=true (off).
      // Act
      useAudioControlStore.getState().toggleMonitor();
      // Assert
      const s = useAudioControlStore.getState();
      expect(s.muted).toBe(false);
      expect(s.listenEnabled).toBe(true);
    });

    it('turns monitor off when currently listening', () => {
      // Arrange — monitor on.
      useAudioControlStore.setState({ listenEnabled: true, muted: false });
      // Act
      useAudioControlStore.getState().toggleMonitor();
      // Assert — muted flips on; listen intent is retained, not cleared.
      const s = useAudioControlStore.getState();
      expect(s.muted).toBe(true);
      expect(s.listenEnabled).toBe(true);
    });

    it('cycles on → off → on across repeated toggles', () => {
      // Act — three toggles from the initial off state.
      useAudioControlStore.getState().toggleMonitor(); // on
      useAudioControlStore.getState().toggleMonitor(); // off
      useAudioControlStore.getState().toggleMonitor(); // on
      // Assert
      const s = useAudioControlStore.getState();
      expect(s.muted).toBe(false);
      expect(s.listenEnabled).toBe(true);
    });

    it('drives to a coherent on state from a legacy partial state', () => {
      // Arrange — a state that could arise if volume was dragged up before any
      // explicit monitor toggle: listenEnabled=false but muted=false.
      useAudioControlStore.setState({ listenEnabled: false, muted: false });
      // Act
      useAudioControlStore.getState().toggleMonitor();
      // Assert — toggle collapses to one coherent "on" state.
      const s = useAudioControlStore.getState();
      expect(s.listenEnabled).toBe(true);
      expect(s.muted).toBe(false);
    });
  });

  describe('enableMonitor', () => {
    it('forces the browser listen state to on and preserves HW gates', () => {
      // Arrange — monitor off AND device HW gates on (as if the user had
      // stopped listening, then navigates away and back into the media page).
      useAudioControlStore.setState({
        listenEnabled: false,
        muted: true,
        captureAvailable: true,
        playbackEnabled: true,
      });
      // Act — the audio-owning player calls enableMonitor on mount to honor
      // "喇叭默认开启".
      useAudioControlStore.getState().enableMonitor();
      // Assert — listen intent is on, but the device HW gates (which mirror
      // real device state owned by the Peripheral page) are left untouched.
      const s = useAudioControlStore.getState();
      expect(s.listenEnabled).toBe(true);
      expect(s.muted).toBe(false);
      expect(s.captureAvailable).toBe(true);
      expect(s.playbackEnabled).toBe(true);
    });
  });
});
