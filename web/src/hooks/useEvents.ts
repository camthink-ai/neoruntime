import { useCallback, useEffect, useRef, useState } from 'react';
import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { eventsApi } from '@/services/api';

/** Topic 信息 */
export interface TopicInfo {
  name: string;
  subscriber_count: number;
}

/** 实时事件 */
export interface LiveEvent {
  id: string;
  topic: string;
  payload: Record<string, unknown>;
  timestamp: number;
}

// ─── 主题列表 ───────────────────────────────────────────────

export const useEventTopics = () => useQuery({
    queryKey: ['events', 'topics'],
    queryFn: async () => {
      const res = await eventsApi.listTopics();
      return (res.data?.topics ?? []) as TopicInfo[];
    },
  });

// ─── 发布事件 ───────────────────────────────────────────────

export const usePublishEvent = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async ({
      topic,
      payload,
    }: {
      topic: string;
      payload?: Record<string, unknown>;
    }) => {
      const res = await eventsApi.publish(topic, payload);
      return res.data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['events', 'topics'] });
    },
  });
};

// ─── 订阅 / 取消订阅 ───────────────────────────────────────
// 订阅状态由前端本地维护：实际事件接收由 useEventStream 通过
// WebSocket /api/v1/events/stream?topic=... 完成，后端无 HTTP 订阅接口。

export const useSubscribeTopic = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (_topic: string) => {},
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['events', 'topics'] });
    },
  });
};

export const useUnsubscribeTopic = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (_topic: string) => {},
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['events', 'topics'] });
    },
  });
};

// ─── WebSocket 实时事件流 ───────────────────────────────────

export const useEventStream = (subscribedTopics?: string[]) => {
  const [events, setEvents] = useState<LiveEvent[]>([]);
  const [connected, setConnected] = useState(false);
  const wsRef = useRef<WebSocket | null>(null);
  const idCounter = useRef(0);
  const topicsRef = useRef(subscribedTopics);
  topicsRef.current = subscribedTopics;

  const connect = useCallback(() => {
    if (wsRef.current) {
      wsRef.current.close();
    }

    const url = eventsApi.getStreamUrl(topicsRef.current);
    const ws = new WebSocket(url);
    wsRef.current = ws;

    ws.onopen = () => setConnected(true);

    ws.onmessage = e => {
      try {
        const data = JSON.parse(e.data);
        // payload may be a JSON string from the backend
        let { payload } = data;
        if (typeof payload === 'string') {
          try {
            payload = JSON.parse(payload);
          } catch {
            payload = { raw: payload };
          }
        }
        const event: LiveEvent = {
          id: String(++idCounter.current),
          topic: data.topic ?? '',
          payload: payload ?? data,
          timestamp: data.timestamp_ns
            ? Math.floor(data.timestamp_ns / 1_000_000)
            : Date.now(),
        };
        setEvents(prev => [event, ...prev].slice(0, 200));
      } catch {
        // ignore non-JSON messages
      }
    };

    ws.onclose = () => setConnected(false);
    ws.onerror = () => setConnected(false);
  }, []);

  const disconnect = useCallback(() => {
    wsRef.current?.close();
    wsRef.current = null;
    setConnected(false);
  }, []);

  const reconnect = useCallback(() => {
    disconnect();
    connect();
  }, [disconnect, connect]);

  const clearEvents = useCallback(() => setEvents([]), []);

  useEffect(() => {
    connect();
    return () => disconnect();
  }, [connect, disconnect]);

  return { events, connected, connect, disconnect, reconnect, clearEvents };
};
