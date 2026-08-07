export interface Container {
  id: string;
  name: string;
  image: string;
  status: 'running' | 'stopped' | 'paused' | 'error';
  cpu: number;
  memory: number;
  network: string;
  ports: string[];
  createdAt: string;
  updatedAt: string;
}

export interface ContainerApp {
  id: string;
  name: string;
  description: string;
  icon: string;
  version: string;
  category: string;
  author: string;
  size: string;
  installed: boolean;
  containerId?: string;
  status?: 'running' | 'stopped';
}

export interface ImportAppPayload {
  name: string;
  image: string;
  ports?: string[];
  env?: Record<string, string>;
  volumes?: string[];
}
