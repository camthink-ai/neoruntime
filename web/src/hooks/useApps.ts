import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { appsApi } from '@/services/api';
import type { WizardConfig, AppPermissions } from '@/services/types';

export interface InstallProgress {
  task_id: string;
  phase: string;
  percent: number;
  message: string;
  app_id: string;
  error: string;
}

export interface AppManifest {
  manifest_path?: string;
  image_path?: string;
  image?: string;
}

export const useApps = (options?: { refetchInterval?: number | false }) => useQuery({
    queryKey: ['apps'],
    queryFn: async () => {
      const response = await appsApi.list();
      return response.data;
    },
    refetchInterval: options?.refetchInterval ?? false,
  });

export const useAppInfo = (appId: string | null) => useQuery({
    queryKey: ['apps', appId],
    queryFn: async () => {
      if (!appId) throw new Error('App ID is required');
      const response = await appsApi.get(appId);
      return response.data;
    },
    enabled: !!appId,
  });

export const useAppPermissions = (appId: string | null | undefined) => useQuery<AppPermissions | null>({
    queryKey: ['apps', appId, 'permissions'],
    queryFn: async () => {
      if (!appId) return null;
      try {
        const response = await appsApi.getPermissions(appId);
        return response.data as AppPermissions;
      } catch {
        return null;
      }
    },
    enabled: !!appId,
    staleTime: 60000,
  });

export const useAppStats = (appId: string | null | undefined) => useQuery({
    queryKey: ['apps', appId, 'stats'],
    queryFn: async () => {
      if (!appId) return null;
      try {
        const response = await appsApi.getStats(appId);
        return response.data;
      } catch {
        // App may have been deleted, return null silently
        return null;
      }
    },
    enabled: !!appId,
    refetchInterval: 5000,
    retry: false,
  });

export const useAppLogs = (appId: string, lines: number = 100) => useQuery({
    queryKey: ['apps', appId, 'logs', lines],
    queryFn: async () => {
      const response = await appsApi.getLogs(appId, { max_lines: lines });
      return response.data;
    },
    enabled: !!appId,
  });

export const useInstallApp = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      // 从应用详情获取 manifest_path 和 image_path
      const detailResponse = await appsApi.get(appId);
      const appDetail = detailResponse?.data;

      if (!appDetail) {
        throw new Error('无法获取应用详情');
      }

      // 检查必需的字段
      if (!appDetail.manifest_path) {
        throw new Error('应用缺少 manifest_path 信息');
      }

      const response = await appsApi.install({
        manifest_path: appDetail.manifest_path,
        image_path: appDetail.image_path || '',
      });
      return response.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useWizardInstall = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (config: WizardConfig) => {
      const response = await appsApi.wizardInstall(config);
      return response.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useUninstallApp = () => {
  const queryClient = useQueryClient();

  return useMutation({
    onMutate: async appId => {
      // Cancel any ongoing queries for this app
      await queryClient.cancelQueries({ queryKey: ['apps', appId] });
      // Remove the app from all related caches immediately
      queryClient.removeQueries({ queryKey: ['apps', appId] });
    },
    mutationFn: async (appId: string) => {
      const response = await appsApi.uninstall(appId);
      return response.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useStartApp = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      const response = await appsApi.start(appId);
      return response.data;
    },
    onSuccess: (_, appId) => {
      queryClient.invalidateQueries({ queryKey: ['apps', appId] });
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useStopApp = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      const response = await appsApi.stop(appId);
      return response.data;
    },
    onSuccess: (_, appId) => {
      queryClient.invalidateQueries({ queryKey: ['apps', appId] });
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useRestartApp = () => {
  const queryClient = useQueryClient();

  return useMutation({
    mutationFn: async (appId: string) => {
      const response = await appsApi.restart(appId);
      return response.data;
    },
    onSuccess: (_, appId) => {
      queryClient.invalidateQueries({ queryKey: ['apps', appId] });
      queryClient.invalidateQueries({ queryKey: ['apps'] });
      queryClient.invalidateQueries({ queryKey: ['store', 'apps'] });
      queryClient.invalidateQueries({ queryKey: ['containers'] });
    },
  });
};

export const useInstallProgress = (taskId: string | null) => useQuery<InstallProgress>({
    queryKey: ['install-progress', taskId],
    queryFn: async () => {
      if (!taskId) throw new Error('Task ID is required');
      const response = await appsApi.getInstallProgress(taskId);
      return response.data as InstallProgress;
    },
    enabled: !!taskId,
    refetchInterval: query => {
      const { data } = query.state;
      if (data?.phase === 'complete' || data?.phase === 'error') return false;
      return 1500;
    },
  });
