import request from '@/services/request';
import type { WizardConfig } from '@/services/types';

// Apps API
export const appsApi = {
  // 获取应用列表（静默请求）
  list: () => request.get('/api/v1/apps', { silent: true } as any),

  // 获取应用详情
  get: (appId: string) => request.get(`/api/v1/apps/${appId}`),

  // 获取应用统计（静默请求，失败不显示错误提示）
  getStats: (appId: string) => request.get(`/api/v1/apps/${appId}/stats`, { silent: true } as any),

  // 获取应用权限
  getPermissions: (appId: string) => request.get(`/api/v1/apps/${appId}/permissions`, { silent: true } as any),

  // 获取应用日志
  getLogs: (appId: string, params?: { max_lines?: number; follow?: boolean }) => request.get(`/api/v1/apps/${appId}/logs`, { params }),

  // 启动应用
  start: (appId: string) => request.post(`/api/v1/apps/${appId}/start`),

  // 停止应用
  stop: (appId: string, timeout?: number) => request.post(`/api/v1/apps/${appId}/stop`, null, { params: { timeout } }),

  // 重启应用
  restart: (appId: string) => request.post(`/api/v1/apps/${appId}/restart`),

  // 卸载应用
  uninstall: (appId: string, keepLogs?: boolean) => request.delete(`/api/v1/apps/${appId}`, {
      params: { keep_logs: keepLogs },
    }),

  // 安装应用
  install: (data: { manifest_path: string; image_path?: string }) => request.post('/api/v1/apps', data),

  // 向导式安装应用（传递配置对象，后端生成 manifest）
  wizardInstall: (config: WizardConfig) => request.post('/api/v1/apps/wizard', config),

  // 获取异步安装进度
  getInstallProgress: (taskId: string) => request.get(`/api/v1/apps/install-progress/${taskId}`),

  // 上传镜像文件
  uploadImage: (file: File, onProgress?: (progress: number) => void) => {
    const formData = new FormData();
    formData.append('file', file); // 字段名必须是 'file'

    return request.post('/api/v1/apps/upload-image', formData, {
      headers: {
        'Content-Type': 'multipart/form-data',
      },
      onUploadProgress: progressEvent => {
        if (onProgress && progressEvent.total) {
          const percentCompleted = Math.round(
            (progressEvent.loaded * 100) / progressEvent.total
          );
          onProgress(percentCompleted);
        }
      },
    });
  },

  // 上传 app.yaml 清单文件
  uploadManifest: (file: File) => {
    const formData = new FormData();
    formData.append('file', file);
    return request.post('/api/v1/apps/upload-manifest', formData, {
      headers: { 'Content-Type': 'multipart/form-data' },
    });
  },

  // 从已上传的清单+镜像安装应用（异步，返回 task_id）
  installPackage: (data: { manifest_path: string; image_path?: string }) => request.post('/api/v1/apps/install-package', data),
};
