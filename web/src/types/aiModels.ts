export interface AiModelTemplate {
  id: string;
  name: string;
  description: string;
  version: string;
  sizeMB: number;
  author: string;
  category: string;
}

export interface InstalledAiModel {
  id: string;
  name: string;
  image: string;
  status: 'running' | 'stopped' | 'paused';
  cpuUsage: number;
  memoryUsageMB: number;
  memoryTotalGB: number;
}
