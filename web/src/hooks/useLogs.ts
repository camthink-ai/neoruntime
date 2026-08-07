import { useQuery } from '@tanstack/react-query';
import { logsApi } from '@/services/api';

export const useSystemLogs = (lines?: number, _level?: string) => useQuery({
    queryKey: ['logs', 'system', lines ?? 100],
    queryFn: async () => {
      // TODO: Add level filter when backend supports it
      const response = await logsApi.listServices();
      return response.data;
    },
    refetchInterval: 5000,
  });

export const useServiceLogs = (service: string, _lines: number = 100) => useQuery({
    queryKey: ['logs', 'service', service],
    queryFn: async () => {
      const response = await logsApi.listFiles();
      return response.data;
    },
    enabled: !!service,
    refetchInterval: 5000,
  });
