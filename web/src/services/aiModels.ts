import type { AiModelTemplate, InstalledAiModel } from '@/types/aiModels';

// Mock functions for compatibility
export const getAiModelTemplates = (): Promise<AiModelTemplate[]> => Promise.resolve([]);

export const getInstalledAiModels = (): Promise<InstalledAiModel[]> => Promise.resolve([]);

export const installAiModel = (_id: string): Promise<void> => Promise.resolve();
