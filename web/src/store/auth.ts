import { create } from 'zustand';
import { getItem, setItem, removeItem } from '@/utils/storage';
import { isCriticalDeviceTransitionActive } from '@/utils/deviceTransition';

interface AuthState {
  isValidateToken: boolean;
}

interface AuthActions {
  setToken: (token: string) => void;
  clearToken: () => void;
}

type AuthStore = AuthState & AuthActions;

export const enableAuth = import.meta.env.VITE_ENABLE_AUTH !== 'false';
const AUTH_SESSION_CHECK_TIMEOUT = 5000;

export function hasValidAuthToken(): boolean {
  if (!enableAuth) return true;
  return Boolean(getItem('token'));
}

export function isLoginPath(pathname: string): boolean {
  return pathname === '/login';
}

export function shouldRedirectToLogin(pathname: string): boolean {
  return enableAuth && !isLoginPath(pathname) && !hasValidAuthToken();
}

export function redirectToLoginBeforeRender(): void {
  if (typeof window === 'undefined') return;
  if (!shouldRedirectToLogin(window.location.pathname)) return;
  window.history.replaceState(null, '', '/login');
}
export async function validateAuthSession(
  signal?: AbortSignal
): Promise<boolean> {
  if (!hasValidAuthToken()) return false;

  const token = getItem<string>('token');
  if (!token) return false;

  const controller = new AbortController();
  const abortRequest = () => controller.abort();
  const timeout = window.setTimeout(abortRequest, AUTH_SESSION_CHECK_TIMEOUT);
  signal?.addEventListener('abort', abortRequest, { once: true });

  try {
    const response = await fetch('/api/v1/system/info', {
      headers: { Authorization: token },
      cache: 'no-store',
      signal: controller.signal,
    });

    if (response.status === 401 || response.status === 403) {
      if (isCriticalDeviceTransitionActive()) {
        return true;
      }
      clearAuthToken();
      return false;
    }

    return true;
  } catch {
    return true;
  } finally {
    window.clearTimeout(timeout);
    signal?.removeEventListener('abort', abortRequest);
  }
}

export const useAuthStore = create<AuthStore>(set => ({
  isValidateToken: hasValidAuthToken(),

  setToken: (token: string) => {
    setItem('token', token);
    set({ isValidateToken: true });
  },

  clearToken: () => {
    removeItem('token');
    removeItem('username');
    set({ isValidateToken: !enableAuth });
  },
}));

export const clearAuthToken = () => {
  removeItem('token');
  useAuthStore.setState({ isValidateToken: !enableAuth });
};

export const authStore = useAuthStore.getState;
