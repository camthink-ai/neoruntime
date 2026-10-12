import { useQuery } from '@tanstack/react-query';

import { systemApi } from '@/services/api/system';

interface UseAppCompatOptions {
  /** Poll interval in ms; the takeover page sets this to self-heal after an
   *  in-place app upgrade without waiting for a reboot-induced reload. */
  refetchInterval?: number;
}

// Shared installed-app compatibility verdict. The verdict only changes
// across an app install plus reboot, which reloads the page anyway; a
// single fetch per session is enough for guards and the banner.
export function useAppCompat(options: UseAppCompatOptions = {}) {
  return useQuery({
    queryKey: ['app-compat'],
    queryFn: async () => {
      const response = await systemApi.appCompatStatus();
      return response.data;
    },
    staleTime: Infinity,
    retry: 1,
    refetchOnWindowFocus: false,
    refetchInterval: options.refetchInterval,
  });
}
