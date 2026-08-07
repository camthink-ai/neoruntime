import request from '@/services/request';

// Streams API
export const streamsApi = {
  // 获取码流列表
  // 后端统一接口：GET /api/v1/media/status
  list: () => request.get('/api/v1/media/status'),

  // 获取码流信息
  // 当前无单独的 /streams/:id 接口，复用 status 由调用方自行筛选
  get: (streamId: string) => request.get('/api/v1/media/status', { params: { stream_id: streamId } }),
};
