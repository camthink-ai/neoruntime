import request from '@/services/request';
import type { LogType, LogStreamOptions } from '@/types/log';

// Logs API
export const logsApi = {
  // 获取日志服务列表
  listServices: () => request.get('/api/v1/logs/services'),

  // 获取日志文件列表
  listFiles: () => request.get('/api/v1/logs/files'),

  // 获取日志内容 (支持服务日志和文件日志)
  getContent: (type: LogType, target: string, lines?: number) => request.get('/api/v1/logs/content', {
      params: { type, target, lines },
    }),

  // 下载日志
  download: (type: LogType, target: string): Promise<Blob> => request.get('/api/v1/logs/download', {
      params: { type, target },
      responseType: 'blob',
    }) as Promise<Blob>,

  // 创建 WebSocket 日志流连接
  createLogStream: (options: LogStreamOptions): WebSocket => {
    const { type, target, token, onMessage, onError, onClose } = options;

    // 构建 WebSocket URL
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const { host } = window.location;
    const params = new URLSearchParams({ type, target });
    if (token) {
      params.append('token', token);
    }

    const wsUrl = `${protocol}//${host}/api/v1/logs/stream/ws?${params.toString()}`;

    const ws = new WebSocket(wsUrl);

    ws.onmessage = event => {
      if (onMessage) {
        onMessage(event.data);
      }
    };

    ws.onerror = error => {
      console.error('[logsApi] WebSocket error:', error);
      if (onError) {
        onError(error);
      }
    };

    ws.onclose = event => {
      console.log('[logsApi] WebSocket closed:', {
        code: event.code,
        reason: event.reason,
      });
      if (onClose) {
        onClose();
      }
    };

    ws.onopen = () => {
      console.log('[logsApi] WebSocket connection opened successfully');
    };

    return ws;
  },
};
