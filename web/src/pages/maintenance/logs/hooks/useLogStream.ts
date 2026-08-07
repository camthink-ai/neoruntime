import { useEffect, useRef, useState } from 'react';
import { logsApi } from '@/services/api/logs';
import { getItem } from '@/utils/storage';
import type { LogType } from '@/types/log';

export interface LogStreamOptions {
  type: LogType;
  target: string;
  enabled?: boolean;
  onNewLine?: (line: string) => void;
}

export const useLogStream = ({
  type,
  target,
  enabled = false,
  onNewLine,
}: LogStreamOptions) => {
  const wsRef = useRef<WebSocket | null>(null);
  const [isConnected, setIsConnected] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!enabled || !target) {
      // 关闭现有连接
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
        setIsConnected(false);
      }
      return;
    }

    const token = getItem<string>('token');
    console.log('[useLogStream] Creating WebSocket connection:', {
      type,
      target,
      hasToken: !!token,
    });

    // 创建 WebSocket 连接
    const ws = logsApi.createLogStream({
      type,
      target,
      token: token || undefined,
      onMessage: data => {
        console.log('[useLogStream] Received message from WebSocket:', data);
        if (onNewLine) {
          onNewLine(data);
        }
      },
      onError: err => {
        console.error('[useLogStream] WebSocket error:', err);
        setError('WebSocket connection error');
        setIsConnected(false);
      },
      onClose: () => {
        console.log('[useLogStream] WebSocket connection closed');
        setIsConnected(false);
      },
    });

    ws.onopen = () => {
      console.log('[useLogStream] WebSocket connection opened');
      setIsConnected(true);
      setError(null);
    };

    wsRef.current = ws;

    // 清理函数
    return () => {
      console.log('[useLogStream] Cleaning up WebSocket connection');
      if (
        ws.readyState === WebSocket.OPEN
        || ws.readyState === WebSocket.CONNECTING
      ) {
        ws.close();
      }
      setIsConnected(false);
    };
  }, [type, target, enabled, onNewLine]);

  const disconnect = () => {
    if (wsRef.current) {
      wsRef.current.close();
      wsRef.current = null;
      setIsConnected(false);
    }
  };

  return {
    isConnected,
    error,
    disconnect,
  };
};
