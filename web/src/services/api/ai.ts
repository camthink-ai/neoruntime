import request from '@/services/request';

// AI Models API
export const aiApi = {
  // Platform capabilities (supported formats + model types)
  getCapabilities: () => request.get('/api/v1/ai/capabilities'),

  // Parse model file (step 1: upload + extract metadata)
  parseModel: (formData: FormData) => request.post('/api/v1/ai/models/parse', formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
    }),

  // Register model from parsed result (step 2: confirm + register)
  registerModel: (data: {
    file_hash: string;
    model_id: string;
    model_type: string;
    model_variant: string;
    config: Record<string, unknown>;
    file_size: number;
    network_name: string;
    vstream_info: string;
    input_width: number;
    input_height: number;
  }) => request.post('/api/v1/ai/models', data),

  // Legacy: register by path
  register: (modelPath: string, modelId?: string) => request.post('/api/v1/ai/models', {
      model_path: modelPath,
      model_id: modelId,
    }),

  // Legacy: upload + register in one step
  upload: (formData: FormData) => request.post('/api/v1/ai/models/upload', formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
    }),

  // Scan disk for new models
  scanModels: () => request.post('/api/v1/ai/models/scan'),

  // 获取模型列表
  list: () => request.get('/api/v1/ai/models'),

  // 获取模型信息
  get: (modelId: string) => request.get(`/api/v1/ai/models/${modelId}`),

  // 注销模型
  unregister: (modelId: string) => request.delete(`/api/v1/ai/models/${modelId}`),

  // 加载模型到 NPU
  loadModel: (modelId: string) => request.post(`/api/v1/ai/models/${modelId}/load`),

  // 从 NPU 卸载模型
  unloadModel: (modelId: string) => request.post(`/api/v1/ai/models/${modelId}/unload`),

  // 获取 AI 统计
  getStats: () => request.get('/api/v1/ai/stats'),
};
