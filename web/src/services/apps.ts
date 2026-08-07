import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import type { AppTemplate, InstalledApp, AppDetailResponse } from './types';
import {
  mockAppTemplates,
  mockInstalledApps,
  mockAppDashboardStats,
} from './mockData';
import { appsApi } from '@/services/api';

const USE_MOCK_DATA = import.meta.env.VITE_USE_MOCK_DATA === 'true';

// Simulated API delay
const delay = (ms: number) => new Promise(resolve => {
    setTimeout(resolve, ms);
  });

// 应用模板列表（原商店应用列表）
export const useAppTemplates = () => useQuery<AppTemplate[]>({
    queryKey: ['appTemplates'],
    queryFn: async () => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return [...mockAppTemplates];
      }

      // 调用应用列表接口获取可用应用模板
      const response = await appsApi.list();
      const apps = response?.data || [];

      // 转换为 AppTemplate 格式
      return apps.map((app: any) => ({
        id: app.id || app.app_id,
        name: app.name,
        description: app.description || '',
        category: app.category || 'Other',
        version: app.version || '1.0.0',
        author: app.author || 'Unknown',
        iconBgColor: 'bg-blue-100',
        iconColor: 'text-blue-600',
        isInstalled: false,
      }));
    },
  });

// 应用详情
export const useAppDetail = (appId: string | null) => useQuery<AppDetailResponse>({
    queryKey: ['appDetail', appId],
    queryFn: async () => {
      if (!appId) {
        throw new Error('App ID is required');
      }

      const response = await appsApi.get(appId);
      return response?.data;
    },
    enabled: !!appId,
  });

export const useInstalledApps = () => useQuery<InstalledApp[]>({
    queryKey: ['installedApps'],
    queryFn: async () => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return [...mockInstalledApps];
      }

      // 调用应用列表接口
      const response = await appsApi.list();
      const apps = response?.data;

      // 转换 API 数据为 InstalledApp 格式
      if (apps && Array.isArray(apps)) {
        return apps.map((app: any) => ({
          id: app.id || app.app_id,
          name: app.name || app.id,
          version: app.version || '1.0.0',
          status: app.status || 'stopped',
          image: app.image || '',
          cpuUsage: app.cpu_usage || 0,
          memoryUsage: app.memory_usage || 0,
          memoryUsageMB: app.memory_usage
            ? Math.round(app.memory_usage / 1024 / 1024)
            : 0,
          memoryTotalGB: 4, // Default value, should come from system info
          uptime: app.uptime || 0,
        }));
      }

      return [...mockInstalledApps];
    },
    refetchInterval: 5000, // 5秒刷新一次
  });

export const useDashboardStats = () => useQuery({
    queryKey: ['dashboardStats'],
    queryFn: async () => {
      await delay(500);
      return mockAppDashboardStats;
    },
  });

// 快速安装应用（从模板一键安装）
export const useInstallAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      if (USE_MOCK_DATA) {
        await delay(800);
        return appId;
      }

      // 从应用详情获取 manifest_path 和 image_path
      const detailResponse = await appsApi.get(appId);
      const appDetail = detailResponse?.data?.app;

      if (!appDetail) {
        throw new Error('无法获取应用详情');
      }

      // 检查必需的字段
      if (!appDetail.manifest_path) {
        throw new Error('应用缺少 manifest_path 信息');
      }

      // 调用 install API
      const response = await appsApi.install({
        manifest_path: appDetail.manifest_path,
        image_path: appDetail.image_path || '',
      });
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['appTemplates'] });
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

// 向导式安装应用（需要完整配置）
export const useWizardInstallMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (config: any) => {
      if (USE_MOCK_DATA) {
        await delay(800);
        return config;
      }

      // 使用 wizardInstall API 安装应用
      const response = await appsApi.wizardInstall(config);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['appTemplates'] });
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

export const useImportAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (data: {
      manifest_path: string;
      image_path?: string;
    }) => {
      if (USE_MOCK_DATA) {
        await delay(1000);
        return data;
      }

      const response = await appsApi.install(data);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

export const useStartAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return appId;
      }

      const response = await appsApi.start(appId);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

export const useStopAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async ({
      appId,
      timeout,
    }: {
      appId: string;
      timeout?: number;
    }) => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return appId;
      }

      const response = await appsApi.stop(appId, timeout);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

export const useRestartAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return appId;
      }

      const response = await appsApi.restart(appId);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

export const useUninstallAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      if (USE_MOCK_DATA) {
        await delay(500);
        return appId;
      }

      const response = await appsApi.uninstall(appId);
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};
// 更新应用（force install — 保留 InstancePath 和运行状态）
export const useUpdateAppMutation = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async ({
      appId,
      manifestPath,
      imagePath,
    }: {
      appId: string;
      manifestPath: string;
      imagePath: string;
    }) => {
      if (USE_MOCK_DATA) {
        await delay(800);
        return { app_id: appId, updated: true };
      }
      const response = await appsApi.install({
        manifest_path: manifestPath,
        image_path: imagePath,
      });
      return response?.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['installedApps'] });
    },
  });
};

// 获取应用统计信息
export const useAppStats = (appId: string | null) => useQuery({
    queryKey: ['appStats', appId],
    queryFn: async () => {
      if (!appId) {
        throw new Error('App ID is required');
      }

      if (USE_MOCK_DATA) {
        await delay(300);
        return {
          cpu_usage: Math.random() * 100,
          memory_usage: Math.random() * 1024 * 1024 * 1024,
          disk_usage: Math.random() * 1024 * 1024 * 1024,
          network_rx: Math.random() * 1024 * 1024,
          network_tx: Math.random() * 1024 * 1024,
        };
      }

      const response = await appsApi.getStats(appId);
      return response?.data;
    },
    enabled: !!appId,
    refetchInterval: 3000, // 3秒刷新一次
  });

// 获取应用日志
export const useAppLogs = (
  appId: string | null,
  params?: { max_lines?: number; follow?: boolean }
) => useQuery({
    queryKey: ['appLogs', appId, params],
    queryFn: async () => {
      if (!appId) {
        throw new Error('App ID is required');
      }

      if (USE_MOCK_DATA) {
        await delay(300);
        return [
          '2024-03-23 10:00:00 [INFO] Application started',
          '2024-03-23 10:00:01 [INFO] Loading configuration',
          '2024-03-23 10:00:02 [INFO] Connecting to database',
          '2024-03-23 10:00:03 [INFO] Server listening on port 8080',
        ];
      }

      const response = await appsApi.getLogs(appId, params);
      return response?.data;
    },
    enabled: !!appId,
    refetchInterval: params?.follow ? 2000 : false, // 如果开启 follow，每2秒刷新
  });
