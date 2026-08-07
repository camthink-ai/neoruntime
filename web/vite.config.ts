import { defineConfig, loadEnv, type PluginOption } from 'vite';
import react from '@vitejs/plugin-react';
import tailwindcss from '@tailwindcss/vite';
import mkcert from 'vite-plugin-mkcert';
import path from 'path';
import createPlugins from './vitePlugins';

export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), '');
  const enableHttps = env.VITE_HTTPS === 'true';

  return {
    base: env.VITE_APP_BASE || '/',
    plugins: [
      react(),
      tailwindcss(),
      // Local HTTPS with a trusted self-signed cert (mkcert). Opt-in via
      // VITE_HTTPS=true so the default dev flow is unchanged. Needed to test
      // secure-only browser APIs (e.g. clipboard, crypto.subtle) and the
      // https web console flow locally.
      ...(enableHttps ? [mkcert()] : []),
      ...createPlugins,
    ] as unknown as PluginOption[],
    server: {
      host: '0.0.0.0', // Listen on all network interfaces
      port: 5174,
      proxy: {
        '/api': {
          target: env.VITE_API_TARGET || 'http://localhost:8080',
          changeOrigin: true,
          secure: false,
          ws: true, // Enable WebSocket proxy
        },
      },
    },
    resolve: {
      alias: {
        '@': path.resolve(__dirname, './src'),
      },
    },
    esbuild: {
      jsx: 'automatic',
    },
    optimizeDeps: {
      include: ['react', 'react-dom', 'zustand'],
    },
  };
});
