import { StrictMode } from 'react';
import { createRoot } from 'react-dom/client';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { ReactQueryDevtools } from '@tanstack/react-query-devtools';
import { Toaster } from 'sonner';
import '@/styles/index.css';
import '@/i18n/config';
import App from './App.tsx';
// eslint-disable-next-line import/no-unresolved
// @ts-expect-error: render icons
import 'virtual:svg-icons-register';

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      // Don't retry 5xx server errors �?they are not transient, and retrying
      // amplifies a single failure into 3-4 requests (each re-entering the
      // response interceptor and stacking "服务器内部错�? toasts).
      // Still retry network/timeout/429-class transient failures.
      retry: (failureCount, error: any) => {
        if (failureCount >= 2) return false;
        const status = error?.status ?? error?.response?.status;
        if (typeof status === 'number' && status >= 500 && status < 600) return false;
        const bizCode = error?.data?.code;
        if (bizCode === 500 || bizCode === 3002) return false;
        return true;
      },
    },
  },
});

createRoot(document.getElementById('root')!).render(
  <StrictMode>
    <QueryClientProvider client={queryClient}>
      <App />
      <Toaster position="top-center" richColors />
      {import.meta.env.DEV && <ReactQueryDevtools initialIsOpen={false} />}
    </QueryClientProvider>
  </StrictMode>
);
