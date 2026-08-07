import axios from 'axios';
import { getItem } from '@/utils/storage';
import i18n from '@/i18n/config';
import { toast } from 'sonner';
import { debounce } from 'throttle-debounce';
import { clearAuthToken } from '@/store/auth';
import {
  enterCriticalDeviceTransition,
  isCriticalDeviceTransitionActive,
} from '@/utils/deviceTransition';

declare module 'axios' {
  interface AxiosRequestConfig {
    silent?: boolean;
  }
}

const TOAST_PATCHED_FLAG = '__aipc_toast_patched__';
const TOAST_LAST_AT_KEY = '__aipc_last_toast_at__';

const getLastToastAt = () => {
  const w = window as any;
  return typeof w?.[TOAST_LAST_AT_KEY] === 'number'
    ? (w[TOAST_LAST_AT_KEY] as number)
    : 0;
};

const setLastToastAt = (at: number) => {
  const w = window as any;
  w[TOAST_LAST_AT_KEY] = at;
};

// Patch sonner toast once to record last toast time.
// This lets the request layer suppress duplicate global error toasts
// when business code already showed a more specific toast.
const patchToastOnce = () => {
  const w = window as any;
  if (w?.[TOAST_PATCHED_FLAG]) return;
  w[TOAST_PATCHED_FLAG] = true;

  const t = toast as unknown as Record<string, any>;
  const toastTypes = [
    'success',
    'error',
    'info',
    'warning',
    'message',
    'loading',
  ] as const;
  toastTypes.forEach(k => {
    const original = t[k];
    if (typeof original !== 'function') return;
    t[k] = (...args: any[]) => {
      setLastToastAt(Date.now());
      return original(...args);
    };
  });
};

patchToastOnce();

// Per-URL timeout overrides (ms). Falls back to 30s for everything else.
const longTimeTaskMap: Record<string, number> = {
  '/api/v1/system/ota/parse': 1800000, // 30 min — upload
  '/api/v1/system/ota/install': 1800000, // 30 min — upgrade
  '/api/v1/system/os-upgrade/upload': 1800000, // 30 min — upload
  '/api/v1/system/os-upgrade/validate': 1800000, // 30 min — validate
  '/api/v1/system/os-upgrade/install': 1800000, // 30 min — install
  '/api/v1/apps/upload-image': 1800000, // 30 min — image upload
  '/api/v1/apps': 300000, // 5 min — install
  '/api/v1/debug-logs/export': 300000,
  '/api/v1/device/lens/reset-zero': 40000, // 40s — lens calibration
};

const debouncedTimeoutError = debounce(
  2000,
  (msg: string) => {
    toast.error(msg);
  },
  { atBegin: true }
);

const isInternalServerErrorMessage = (msg: unknown) => {
  if (typeof msg !== 'string') return false;
  return /internal server error|服务器内部错误|服务内部错误|http 500/i.test(
    msg
  );
};

/** URLs that may fail with network errors while the device is restarting (no toast). */
const NETWORK_ERROR_SUPPRESS_URL_PATTERNS = [
  '/api/v1/system/health',
  '/api/v1/system/restart',
  '/api/v1/system/ota/',
  '/api/v1/system/os-upgrade/',
];

/** Suppress global request side effects during device restart / upgrade windows. */
export function enterNetworkErrorToastSuppress() {
  return enterCriticalDeviceTransition();
}

function shouldSuppressNetworkErrorToast(config?: {
  url?: string;
  silent?: boolean;
}) {
  if (config?.silent) return true;
  if (isCriticalDeviceTransitionActive()) return true;
  const url = config?.url ?? '';
  return NETWORK_ERROR_SUPPRESS_URL_PATTERNS.some(pattern => url.includes(pattern));
}

const GLOBAL_ERROR_TOAST_DELAY_MS = 180;
const GLOBAL_ERROR_SUPPRESS_WINDOW_MS = 260;

let scheduledGlobalErrorToken = 0;
const scheduleGlobalErrorToast = (msg: string) => {
  const token = Date.now();
  scheduledGlobalErrorToken = token;

  window.setTimeout(() => {
    if (scheduledGlobalErrorToken !== token) return;

    const sinceLastToast = Date.now() - getLastToastAt();
    if (sinceLastToast <= GLOBAL_ERROR_SUPPRESS_WINDOW_MS) return;

    toast.error(msg);
  }, GLOBAL_ERROR_TOAST_DELAY_MS);
};

const request = axios.create({
  baseURL: '/',
  timeout: 20000,
  headers: {
    'Content-Type': 'application/json',
    Authorization: getItem<string>('token') || '',
  },
});

// Request interceptor
request.interceptors.request.use(
  config => {
    // Add token to request header for all requests (except login)
    // Login API will skip this since token doesn't exist yet
    const token = getItem<string>('token');
    if (token) {
      // Token from backend already includes "Bearer " prefix
      config.headers.Authorization = token;
    }

    const username = getItem<string>('username');
    if (username) {
      config.headers['X-User'] = username;
    }

    // If FormData, delete Content-Type to let browser set it automatically
    if (config.data instanceof FormData) {
      delete (config.headers as any)['Content-Type'];
    }

    // Add timestamp to prevent caching
    if (config.method === 'get') {
      config.params = {
        ...config.params,
        _t: Date.now(),
      };
    }
    // Dynamically set timeout per URL
    const url = (config.url || '') as string;
    const matchedTimeout = Object.entries(longTimeTaskMap).find(([p]) => url.includes(p))?.[1];
    if (config.timeout !== 0) {
      config.timeout = matchedTimeout ?? 30000;
    }

    return config;
  },
  error => Promise.reject(error)
);

// Response interceptor
request.interceptors.response.use(
  response => {
    const { data } = response;
    const config = response.config as any;

    // If response is file/binary, return raw data directly
    const contentType = ((response.headers || {}) as any)['content-type'] as
      | string
      | undefined;
    const isBlob = typeof Blob !== 'undefined' && data instanceof Blob;
    const isArrayBuffer =      typeof ArrayBuffer !== 'undefined' && data instanceof ArrayBuffer;
    const isBinaryContentType =      typeof contentType === 'string'
      && /octet-stream|application\/pdf|image\/.+|video\/.+|audio\/.+|zip|gzip/i.test(
        contentType
      );
    const isBinaryResponseType =      config?.responseType === 'blob'
      || config?.responseType === 'arraybuffer'
      || (response.request && response.request.responseType === 'blob')
      || (response.request && response.request.responseType === 'arraybuffer');

    if (
      isBlob
      || isArrayBuffer
      || isBinaryContentType
      || isBinaryResponseType
    ) {
      return data;
    }
    // 检查是否有 code 字段
    if (data && typeof data === 'object') {
      if (data.code === 0) {
        return data;
      }
      // 业务错误：code 不为 0
      // 检查是否为静默请求（不显示错误提示）
      if (config?.silent) {
        return Promise.reject(response);
      }
      if (isCriticalDeviceTransitionActive()) {
        return Promise.reject(response);
      }
      // 后端返回格式: {"code": 1001, "message": "...", "error": {...}}
      const errorMessage = data.error?.detail || data.message;
      const mapped =        i18n.t(`errors.business.${data.code}`, { ns: 'errors' })
        || errorMessage;
      const finalMsg =        mapped || i18n.t('errors.business.default', { ns: 'errors' });
      // 500 / internal-server-error is silent (no toast); other business errors toast.
      if (!(data.code === 500 || isInternalServerErrorMessage(finalMsg))) {
        scheduleGlobalErrorToast(finalMsg);
      }
      return Promise.reject(response);
    }

    // 如果没有 success 字段，直接返回数据（兼容不同的 API 响应格式）
    return response;
  },
  error => {
    if (!error.response) {
      const config = error.config as
        | { url?: string; silent?: boolean }
        | undefined;
      if (shouldSuppressNetworkErrorToast(config)) {
        return Promise.reject(error);
      }
      const errorMessage =        error.message === 'Network Error'
          ? i18n.t('errors.business.NETWORK_ERROR', { ns: 'errors' })
          : String(error.message || error);
      debouncedTimeoutError(errorMessage);
      return Promise.reject(error);
    }
    const config = error.config as
      | { url?: string; silent?: boolean }
      | undefined;
    if (config?.silent) return Promise.reject(error.response);
    if (isCriticalDeviceTransitionActive()) return Promise.reject(error.response);

    const { status } = error.response;
    switch (status) {
      case 400:
      case 405:
      case 409:
      case 422:
      case 429:
      case 501:
      case 502:
      case 503:
      case 504:
      case 507:
        scheduleGlobalErrorToast(
          i18n.t(`errors.http.${status}`, { ns: 'errors' })
            || i18n.t('errors.http.500', { ns: 'errors' })
        );
        return Promise.reject(error.response);
      case 401:
        clearAuthToken();
        if (!window.location.pathname.includes('/login')) {
          toast.error(i18n.t('errors.http.401', { ns: 'errors' }));
          window.location.href = '/login';
        }
        return Promise.reject(error.response);
      case 403:
        toast.error(i18n.t('errors.http.403', { ns: 'errors' }));
        return Promise.reject(error.response);

      case 404:
        toast.error(i18n.t('errors.http.404', { ns: 'errors' }));
        return Promise.reject(error.response);

      case 500:
        // Silent: 500 errors are not surfaced to the user.
        return Promise.reject(error.response);

      default: {
        return Promise.reject(error.response);
      }
    }
  }
);

// Export request methods
export const http = {
  // Write business path directly, e.g., '/login'
  get: (url: string, params?: any) => request.get(url, { params }),
  post: (url: string, data?: any) => request.post(url, data),
  put: (url: string, data?: any) => request.put(url, data),
  delete: (url: string) => request.delete(url),
  patch: (url: string, data?: any) => request.patch(url, data),
};

// Export axios instance
export default request;
