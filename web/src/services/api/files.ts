import request from '@/services/request';

// Files API (Mock for now - implement when backend is ready)
export const filesApi = {
  // 列出文件
  list: (path: string) => request.get('/api/v1/files', { params: { path } }),

  // 列出文件（静默模式，不弹 toast）
  listSilent: (path: string) => request.get('/api/v1/files', { params: { path }, silent: true } as any),

  // 创建目录
  createDirectory: (path: string) => request.post('/api/v1/files/mkdir', { path }),

  // 删除文件
  delete: (path: string) => request.delete('/api/v1/files', { params: { path } }),

  // 批量删除文件
  batchDelete: (paths: string[]) => request.post('/api/v1/files/batch-delete', { paths }),

  // 重命名文件
  rename: (oldPath: string, newPath: string) => request.post('/api/v1/files/rename', {
      old_path: oldPath,
      new_path: newPath,
    }),

  // 上传文件
  upload: (path: string, file: File) => {
    const formData = new FormData();
    formData.append('file', file);
    formData.append('path', path);
    return request.post('/api/v1/files/upload', formData);
  },

  // 下载文件 - 返回 Blob
  download: (path: string): Promise<Blob> => request.get('/api/v1/files/download', {
      params: { path },
      responseType: 'blob',
    }) as Promise<Blob>,

  // 批量下载文件（压缩包）- 返回 Blob，后端打包成 ZIP
  batchDownload: (paths: string[]): Promise<Blob> => request.post(
      '/api/v1/files/batch-download',
      { paths },
      {
        responseType: 'blob',
      }
    ) as Promise<Blob>,

  // 读取文件内容
  readContent: (path: string) => request.get('/api/v1/files/content', { params: { path } }),

  // 读取文件内容（静默模式，由调用方展示错误）
  readContentSilent: (path: string) => request.get('/api/v1/files/content', {
      params: { path },
      silent: true,
    } as any),

  // 写入文件内容
  writeContent: (path: string, content: string) => request.post('/api/v1/files/content', { path, content }),
};
