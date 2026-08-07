import request from '@/services/request';

// Settings API
export const settingsApi = {
  // 获取所有设置
  getAll: () => request.get('/api/v1/settings'),

  // 设置配置
  set: (key: string, value: any, description?: string) => request.post('/api/v1/settings', { key, value, description }),

  // 删除设置
  delete: (key: string) => request.delete(`/api/v1/settings/${key}`),
};
