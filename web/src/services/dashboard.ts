import { useQuery, keepPreviousData } from '@tanstack/react-query';
import type {
  SystemStats,
  Activity,
  SystemInfo,
  PlatformStats,
  NetworkStats,
} from '@/services/types';
import { monitorApi, appsApi, aiApi } from '@/services/api';
import { fetchDeviceInfo, fetchNetworkInterfaces } from '@/services/settings';

export const useDashboardStats = () => useQuery<SystemStats>({
    queryKey: ['dashboard', 'stats'],
    queryFn: async () => {
      const response = await monitorApi.getSummary();
      const data = response?.data;

      if (data) {
        return {
          cpu: {
            usage: parseFloat((data.cpu?.usage_percent || 0).toFixed(2)),
            cores: data.cpu?.cores || 0,
          },
          npu: {
            usage: parseFloat((data.npu || 0).toFixed(2)),
          },
          memory: {
            usageGB: parseFloat(
              ((data.memory?.used || 0) / 1024 / 1024 / 1024).toFixed(2)
            ),
            totalGB: parseFloat(
              ((data.memory?.total || 0) / 1024 / 1024 / 1024).toFixed(2)
            ),
            usagePercent: parseFloat(
              (data.memory?.usage_percent || 0).toFixed(2)
            ),
          },
          storage: {
            usageGB: parseFloat(
              ((data.disk?.used || 0) / 1024 / 1024 / 1024).toFixed(2)
            ),
            totalGB: parseFloat(
              ((data.disk?.total || 0) / 1024 / 1024 / 1024).toFixed(2)
            ),
            usagePercent: parseFloat(
              (data.disk?.usage_percent || 0).toFixed(2)
            ),
            mountpoint: data.disk?.mountpoint || '/',
            type: data.disk?.mountpoint === '/data' ? 'eMMC' : 'SSD',
          },
        };
      }

      throw new Error('Failed to fetch dashboard stats');
    },
    placeholderData: keepPreviousData,
    refetchInterval: 5000,
  });

export const useRecentActivities = () => useQuery<Activity[]>({
    queryKey: ['dashboard', 'activities'],
    queryFn: async () => {
      const response = await appsApi.list();
      const apps = response?.data;

      if (apps && Array.isArray(apps)) {
        const activities: Activity[] = apps.slice(0, 5).map((app: any) => ({
          id: app.id || app.app_id,
          type: app.status === 'running' ? 'app_started' : 'app_stopped',
          message: `${app.name || app.id} ${app.status === 'running' ? 'started' : 'stopped'}`,
          timestamp: new Date().toISOString(),
        }));
        return activities;
      }

      return [];
    },
    refetchInterval: 10000,
  });

export const useSystemInfo = () => useQuery<SystemInfo>({
    queryKey: ['dashboard', 'systemInfo'],
    queryFn: async () => {
      const [deviceData, networkInterfaces] = await Promise.all([
        fetchDeviceInfo(),
        fetchNetworkInterfaces().catch(() => []),
      ]);

      // 从网络接口中获取 IP 地址：优先选择默认接口，其次选择第一个 up 状态的接口
      const defaultInterface = networkInterfaces.find(
        iface => iface.is_default && iface.status === 'up'
      );
      const firstUpInterface = networkInterfaces.find(
        iface => iface.status === 'up'
      );
      const selectedInterface = defaultInterface || firstUpInterface;

      return {
        device_name: deviceData.device_name || '-',
        model: deviceData.model || '-',
        firmware_version: deviceData.firmware_version || '-',
        build_date: deviceData.build_date || '-',
        mac_address:
          selectedInterface?.mac_address || deviceData.mac_address || '-',
        ip_address: selectedInterface?.ip_address || '-',
      };
    },
    placeholderData: keepPreviousData,
    staleTime: 30000,
    refetchInterval: 30000,
  });

export const usePlatformStats = () => useQuery<PlatformStats>({
    queryKey: ['dashboard', 'platformStats'],
    queryFn: async () => {
      const [appsRes, modelsRes] = await Promise.allSettled([
        appsApi.list().catch(() => ({ data: [] })),
        aiApi.list().catch(() => ({ data: [] })),
      ]);

      const unwrapList = (value: any, key?: string): any[] => {
        // request.ts returns `data` directly when { code: 0, data: ... }
        // Backends may return: { code:0, data: [] } or { code:0, data: { apps: [] } }
        const candidate =          (key ? value?.data?.[key] : undefined)
          ?? value?.data
          ?? (key ? value?.[key] : undefined)
          ?? value;
        return Array.isArray(candidate) ? candidate : [];
      };

      const apps =        appsRes.status === 'fulfilled' ? unwrapList(appsRes.value, 'apps') : [];
      const models =        modelsRes.status === 'fulfilled'
          ? unwrapList(modelsRes.value, 'models')
          : [];

      // Apps list should already include metrics when available (cpu_percent/memory_usage/memory_limit).
      const mergedApps = apps;
      const pseudoContainers = mergedApps.map((a: any) => ({
        id: a.container_id || a.id,
        state: a.state || a.status,
      }));

      return {
        apps: {
          total: mergedApps.length,
          running: mergedApps.filter((a: any) => a.state === 'running').length,
          stopped: mergedApps.filter((a: any) => a.state !== 'running').length,
          list: mergedApps,
        },
        containers: {
          total: pseudoContainers.length,
          running: pseudoContainers.filter((c: any) => c.state === 'running')
            .length,
          stopped: pseudoContainers.filter((c: any) => c.state !== 'running')
            .length,
          list: pseudoContainers,
        },
        models: {
          total: models.length,
          loaded: models.filter((m: any) => !!m.load_timestamp).length,
          list: models, // 返回完整的模型列表
        },
        cameras: {
          total: 0,
          online: 0,
          offline: 0,
        },
      };
    },
    placeholderData: keepPreviousData,
  });

export const useNetworkStats = () => useQuery<NetworkStats>({
    queryKey: ['dashboard', 'networkStats'],
    queryFn: async () => {
      const response = await monitorApi.getNetwork();
      const data = response?.data;

      if (data) {
        return {
          rx_bytes: data.rx_bytes || 0,
          tx_bytes: data.tx_bytes || 0,
          rx_rate: data.rx_rate || 0,
          tx_rate: data.tx_rate || 0,
        };
      }

      return {
        rx_bytes: 0,
        tx_bytes: 0,
        rx_rate: 0,
        tx_rate: 0,
      };
    },
    refetchInterval: 5000,
  });

export interface MonitorSnapshot {
  timestamp: number;
  cpu: number;
  memory: number;
  npu: number;
  temperatures: {
    cpu: number;
    npu: number;
    board: number;
  };
  network?: {
    bytes_sent: number;
    bytes_recv: number;
  };
}

// 资源快照：dashboard 内多处共享，避免重复轮询 /api/v1/monitor/snapshot
export const useMonitorSnapshot = () => useQuery<MonitorSnapshot>({
    queryKey: ['monitorSnapshot'],
    queryFn: async () => {
      const response = await monitorApi.getSnapshot();
      return response?.data as MonitorSnapshot;
    },
    refetchInterval: 2000,
    retry: false,
  });
