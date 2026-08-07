import type { MockMethod } from 'vite-plugin-mock';

export default [
  {
    url: '/api/ai/toggle',
    method: 'post',
    timeout: 300,
    response: ({ body }: { body: { enabled?: boolean } }) => {
      const { enabled } = body || {};

      if (typeof enabled !== 'boolean') {
        return {
          success: false,
          code: 400,
          message: 'Invalid parameter',
          data: null,
        };
      }

      return {
        success: true,
        code: 200,
        message: 'AI inference toggled successfully',
        data: {
          enabled,
          status: enabled ? 'running' : 'idle',
        },
      };
    },
  },
  {
    url: '/api/ai/status',
    method: 'get',
    timeout: 200,
    response: () => ({
      success: true,
      code: 200,
      message: 'Success',
      data: {
        enabled: false,
        status: 'idle',
      },
    }),
  },
  {
    url: '/api/ai/capture',
    method: 'post',
    timeout: 300,
    response: () => ({
      success: true,
      code: 200,
      message: 'Photo captured successfully',
      data: {
        timestamp: Date.now(),
        imageUrl: '/mock/captured-image.jpg',
      },
    }),
  },
] as MockMethod[];
