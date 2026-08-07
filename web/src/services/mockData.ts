import type {
  AppTemplate,
  InstalledApp,
  AppDashboardStats,
  SystemStats,
  Activity,
  SystemInfo,
} from './types';

// Re-export types for backward compatibility
export type {
  AppTemplate,
  InstalledApp,
  AppDashboardStats,
  SystemStats,
  Activity,
  SystemInfo,
};

// Apps Mock Data

export const mockAppTemplates: AppTemplate[] = [
  {
    id: 'data-analytics',
    name: 'Data Analytics',
    description: 'Real-time data processing and analytics platform',
    category: 'Analytics',
    version: '2.1.0',
    author: 'Analytics Team',
    iconBgColor: 'bg-blue-100',
    iconColor: 'text-blue-600',
    isInstalled: false,
  },
  {
    id: 'device-mgmt',
    name: 'Device Management',
    description: 'Centralized device monitoring and control system',
    category: 'Management',
    version: '1.5.2',
    author: 'DevOps Team',
    iconBgColor: 'bg-green-100',
    iconColor: 'text-green-600',
    isInstalled: true,
    status: 'running',
    cpuUsage: '12%',
  },
  {
    id: 'security-audit',
    name: 'Security Audit',
    description: 'Comprehensive security scanning and audit tools',
    category: 'Security',
    version: '3.0.1',
    author: 'Security Team',
    iconBgColor: 'bg-red-100',
    iconColor: 'text-red-600',
    isInstalled: false,
  },
];

export const mockInstalledApps: InstalledApp[] = [
  {
    id: 'app-001',
    name: 'Device Management',
    version: '1.5.2',
    status: 'running',
    image: 'device-mgmt:1.5.2',
    cpuUsage: 12.5,
    memoryUsage: 256 * 1024 * 1024,
    memoryUsageMB: 256,
    memoryTotalGB: 4,
    uptime: 86400,
  },
  {
    id: 'app-002',
    name: 'Video Processor',
    version: '2.0.0',
    status: 'running',
    image: 'video-proc:2.0.0',
    cpuUsage: 45.2,
    memoryUsage: 512 * 1024 * 1024,
    memoryUsageMB: 512,
    memoryTotalGB: 4,
    uptime: 43200,
  },
  {
    id: 'app-003',
    name: 'AI Inference',
    version: '1.8.0',
    status: 'stopped',
    image: 'ai-inference:1.8.0',
    cpuUsage: 0,
    memoryUsage: 0,
    memoryUsageMB: 0,
    memoryTotalGB: 4,
    uptime: 0,
  },
];

export const mockAppDashboardStats: AppDashboardStats = {
  totalApps: 12,
  runningApps: 8,
  stoppedApps: 4,
  totalCpu: 45.5,
  totalMemory: 2048,
};

export const mockDashboardStats: SystemStats = {
  cpu: {
    usage: 45.2,
    cores: 4,
  },
  npu: {
    usage: 32.5,
  },
  memory: {
    usageGB: 2.4,
    totalGB: 4.0,
    usagePercent: 60,
  },
  storage: {
    usageGB: 12.5,
    totalGB: 32.0,
    usagePercent: 39,
    type: 'eMMC',
    mountpoint: '/data',
  },
};

export const mockActivities: Activity[] = [
  {
    id: '1',
    type: 'app_started',
    message: 'Device Management started successfully',
    timestamp: new Date(Date.now() - 300000).toISOString(),
  },
  {
    id: '2',
    type: 'app_stopped',
    message: 'AI Inference stopped',
    timestamp: new Date(Date.now() - 600000).toISOString(),
  },
  {
    id: '3',
    type: 'system_update',
    message: 'System updated to version 1.5.0',
    timestamp: new Date(Date.now() - 900000).toISOString(),
  },
];

export const mockSystemInfo: SystemInfo = {
  device_name: 'ne503-device-001',
  model: 'RK3588',
  firmware_version: '1.5.0',
  build_date: '-',
  mac_address: '-',
  ip_address: '-',
};
