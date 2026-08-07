import request from '@/services/request';

export interface SSHConfigResponse {
  data?: {
    config: Record<string, string>;
  };
  config?: Record<string, string>;
}

export interface SSHStatusResponse {
  data?: {
    status: 'active' | 'inactive' | 'unknown';
  };
  status?: 'active' | 'inactive' | 'unknown';
}

export interface SSHLogsResponse {
  data?: {
    logs: string[];
  };
  logs?: string[];
}

export interface SSHConfigRequest {
  port?: string;
  permit_root_login?: 'yes' | 'no' | 'prohibit-password';
  password_authentication?: 'yes' | 'no';
  pubkey_authentication?: 'yes' | 'no';
  max_auth_tries?: string;
  restart_service?: boolean;
}

// SSH API
export const sshApi = {
  // 获取 SSH 配置
  getConfig: (): Promise<SSHConfigResponse> => request.get('/api/v1/ssh/config') as Promise<SSHConfigResponse>,

  // 设置 SSH 配置
  setConfig: (data: SSHConfigRequest): Promise<unknown> => request.post('/api/v1/ssh/config', data),

  // 获取 SSH 状态
  getStatus: (): Promise<SSHStatusResponse> => request.get('/api/v1/ssh/status') as Promise<SSHStatusResponse>,

  // 获取 SSH 日志
  getLogs: (): Promise<SSHLogsResponse> => request.get('/api/v1/ssh/logs') as Promise<SSHLogsResponse>,
};
