import { create } from 'zustand';

interface AiStatusState {
  isAiInference: boolean;
  aiStatus: 'unloaded' | 'loaded' | 'idle' | 'running' | 'error';
}

interface AiStatusActions {
  setIsAiInference: (value: boolean) => void;
  setAiStatus: (
    status: 'unloaded' | 'loaded' | 'idle' | 'running' | 'error'
  ) => void;
}

type AiStatusStore = AiStatusState & AiStatusActions;

export const useAiStatusStore = create<AiStatusStore>(set => ({
  isAiInference: false,
  aiStatus: 'unloaded',

  setIsAiInference: (value: boolean) => {
    set({ isAiInference: value, aiStatus: value ? 'running' : 'idle' });
  },

  setAiStatus: (
    status: 'unloaded' | 'loaded' | 'idle' | 'running' | 'error'
  ) => {
    set({ aiStatus: status });
  },
}));
