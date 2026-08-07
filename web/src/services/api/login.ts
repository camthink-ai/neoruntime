import request from '@/services/request';
import { encryptPassword, invalidatePublicKeyCache } from '@/utils/crypto';

const CODE_INVALID_TIMESTAMP = 1005;

export interface LoginRequest {
  username: string;
  password: string;
}

export interface LoginResponse {
  code: number;
  message: string;
  data: {
    token: string;
    username: string;
  } | null;
  error?: {
    type?: string;
    detail?: string;
  };
}

const isInvalidTimestampError = (err: unknown): boolean => {
  const payload = err as {
    data?: {
      code?: number;
      message?: string;
      error?: { detail?: string };
    };
  } | null;

  const data = payload?.data;
  const detail = data?.error?.detail || data?.message || '';
  return (
    data?.code === CODE_INVALID_TIMESTAMP
    || /request timestamp out of allowed window/i.test(detail)
  );
};

const postLogin = async (params: LoginRequest): Promise<LoginResponse> => {
  const { ciphertext, timestamp } = await encryptPassword(params.password, {
    forcePublicKey: true,
  });
  return request.post(
    '/api/login',
    { username: params.username, password: ciphertext, timestamp },
    { silent: true } as any
  ) as Promise<LoginResponse>;
};

export const authApi = {
  // 登录：密码经设备 RSA 公钥加密后传输，附 unix 秒 timestamp 防重放。
  // timestamp 使用设备时间；若缓存的设备时间源过期，刷新公钥后重试一次。
  login: async (params: LoginRequest): Promise<LoginResponse> => {
    try {
      return await postLogin(params);
    } catch (err) {
      if (!isInvalidTimestampError(err)) {
        throw err;
      }

      invalidatePublicKeyCache();
      return postLogin(params);
    }
  },

  // 登出
  logout: () => request.post('/api/v1/logout'),
};
