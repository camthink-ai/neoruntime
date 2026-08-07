import { useCallback } from 'react';
import { useAuthStore } from '@/store/auth';

export function useLogout() {
  const clearToken = useAuthStore(s => s.clearToken);

  return useCallback(() => {
    clearToken();
    window.location.href = '/login';
  }, [clearToken]);
}
