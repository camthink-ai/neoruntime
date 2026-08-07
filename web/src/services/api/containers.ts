import request from '@/services/request';

// Containers API
export const containersApi = {
  // 获取容器列表（静默请求）
  list: (params?: {
    state?: 'running' | 'stopped' | 'all';
    search?: string;
    page?: number;
    page_size?: number;
  }) => request.get('/api/v1/containers', { params, silent: true } as any),

  // 获取容器详情
  get: (id: string) => request.get(`/api/v1/containers/${id}`),

  // 获取容器统计
  getStats: (id: string) => request.get(`/api/v1/containers/${id}/stats`),

  // 获取容器日志
  getLogs: (id: string, tail?: number) => request.get(`/api/v1/containers/${id}/logs`, { params: { tail } }),

  // 启动容器
  start: (id: string) => request.post(`/api/v1/containers/${id}/start`),

  // 停止容器
  stop: (id: string) => request.post(`/api/v1/containers/${id}/stop`),

  // 重启容器
  restart: (id: string) => request.post(`/api/v1/containers/${id}/restart`),

  // 删除容器
  remove: (id: string) => request.delete(`/api/v1/containers/${id}`),
};
