import { useQuery } from '@tanstack/react-query';
import { logsApi } from '@/services/api/logs';
import { eventLogsApi } from '@/services/api/event_logs';
import type {
  LogFile,
  LogService,
  LogFilterParams,
  LogType,
} from '@/types/log';

// 获取服务列表
export const useLogServices = () => useQuery<LogService[]>({
    queryKey: ['logServices'],
    queryFn: async () => {
      const response = await logsApi.listServices();
      return response.data?.services || [];
    },
  });

// 获取日志文件列表
export const useLogFiles = () => useQuery<LogFile[]>({
    queryKey: ['logFiles'],
    queryFn: async () => {
      const response = await logsApi.listFiles();
      const files = response.data?.files || [];
      return files.map((file: any, index: number) => ({
        id: file.path || `file-${index}`,
        name: file.name || file.path?.split('/').pop() || 'Unknown',
        path: file.path,
        size: file.size || 0,
        modified_time: file.modified_time || '-',
        service: file.service,
      }));
    },
  });

// 获取日志内容
export const useLogContent = (
  type: LogType,
  target: string | null,
  params?: LogFilterParams
) => useQuery({
    queryKey: ['logContent', type, target, params?.lines],
    queryFn: async () => {
      if (!target) return '';
      const response = await logsApi.getContent(
        type,
        target,
        params?.lines || 500
      );
      return response.data?.content || '';
    },
    enabled: !!target,
  });

export type EventTemplates = Record<
  string,
  {
    en: string;
    zh: string;
    module: string;
    level: string;
    params: string[];
  }
>;

export const useEventTemplates = () => useQuery<EventTemplates>({
    queryKey: ['eventTemplates'],
    queryFn: async () => {
      const response = await eventLogsApi.getTemplates();
      return response.data?.templates || {};
    },
    staleTime: Infinity,
  });
