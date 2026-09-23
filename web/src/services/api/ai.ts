import request from '@/services/request';

// AI Models API
export const aiApi = {
  // Platform capabilities (supported formats + model types)
  getCapabilities: () => request.get('/api/v1/ai/capabilities'),

  // Parse model file (step 1: upload + extract metadata).
  // onProgress maps to axios onUploadProgress (percent 0-100); signal aborts
  // the request mid-upload/mid-parse — the server discards an incomplete
  // multipart body on its own.
  parseModel: (
    formData: FormData,
    options?: {
      onProgress?: (percent: number) => void;
      signal?: AbortSignal;
    }
  ) => request.post('/api/v1/ai/models/parse', formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
      onUploadProgress: progressEvent => {
        if (options?.onProgress && progressEvent.total) {
          options.onProgress(
            Math.round((progressEvent.loaded * 100) / progressEvent.total)
          );
        }
      },
      signal: options?.signal,
    }),

  // Abandon a staged blob when the import wizard is cancelled. Server-side
  // reference counting keeps blobs already shared with registered models.
  abandonStaged: (fileHash: string, filePath: string) => request.post('/api/v1/ai/models/parse/abandon', {
      file_hash: fileHash,
      file_path: filePath,
    }),

  // Register model from parsed result (step 2: confirm + register)
  registerModel: (data: {
    file_hash: string;
    model_id: string;
    model_type: string;
    // Output delivery mode: 'platform' (plugin-decoded) | 'raw' (bare tensors)
    output_mode: string;
    model_variant: string;
    config: Record<string, unknown>;
    file_size: number;
    network_name: string;
    vstream_info: string;
    // Omitted (not zero) when the parser could not extract a dimension —
    // UpdateModel's pointer semantics treat a sent 0 as "clear".
    input_width?: number;
    input_height?: number;
  }) => request.post('/api/v1/ai/models', data),

  // Update an existing model (PUT); file_hash empty/omitted = metadata-only
  update: (
    modelId: string,
    data: Partial<{
      file_hash: string;
      model_type: string;
      output_mode: string;
      model_variant: string;
      config: Record<string, unknown>;
      file_size: number;
      network_name: string;
      vstream_info: string;
      input_width: number;
      input_height: number;
    }>
  ) => request.put(`/api/v1/ai/models/${modelId}`, data),

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

  // 导出为 AMPK 单文件包 (.bin)：HEF + 注册元数据
  exportPackage: (modelId: string) => request.get<Blob>(`/api/v1/ai/models/${modelId}/export`, {
      responseType: 'blob',
    }),

  // 获取 AI 统计
  getStats: () => request.get('/api/v1/ai/stats'),
};

/**
 * Download a model as an AMPK .bin package (HEF + registration metadata).
 * Mirrors downloadDebugLogs: blob → object URL → anchor click.
 */
export const downloadModelPackage = async (modelId: string): Promise<void> => {
  const result = await aiApi.exportPackage(modelId);
  // The response interceptor returns data directly for blobs.
  const blob = result as unknown as Blob;
  if (!blob || blob.size === 0) {
    throw new Error('Received empty file');
  }
  const url = window.URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `${modelId}.bin`;
  document.body.appendChild(a);
  a.click();
  window.URL.revokeObjectURL(url);
  document.body.removeChild(a);
};
