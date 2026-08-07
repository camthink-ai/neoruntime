import type { MockMethod } from 'vite-plugin-mock';
import type { CameraDaemonConfig, PtzState } from '../types/media';

let mockMediaConfig: CameraDaemonConfig = {
  encoders: [
    {
      stream_name: 'main',
      codec: 'h264',
      width: 1920,
      height: 1080,
      fps: 30,
      bitrate: 4000000,
      gop: 60,
      enabled: true,
    },
    {
      stream_name: 'sub',
      codec: 'h264',
      width: 1280,
      height: 720,
      fps: 30,
      bitrate: 2000000,
      gop: 60,
      enabled: true,
    },
    {
      stream_name: 'third',
      codec: 'h264',
      width: 640,
      height: 384,
      fps: 15,
      bitrate: 512000,
      gop: 30,
      enabled: true,
    },
  ],
  ai_overlay: {
    enabled: true,
    draw_labels: true,
    draw_confidence: true,
    draw_landmarks: true,
    enable_face_blur: false,
    box_thickness: 2,
  },
  rtsp: { enabled: true },
};

let mockPtzState: PtzState = {
  zoom: 2.4,
  focus: 50,
  mode: 'auto',
};

export default [
  {
    url: '/api/media/config',
    method: 'get',
    response: () => ({
      code: 200,
      data: mockMediaConfig,
      message: 'success',
    }),
  },
  {
    url: '/api/media/config',
    method: 'post',
    response: ({ body }: any) => {
      mockMediaConfig = { ...mockMediaConfig, ...body };
      return {
        code: 200,
        data: mockMediaConfig,
        message: 'success',
      };
    },
  },
  {
    url: '/api/media/ptz/state',
    method: 'get',
    response: () => ({
      code: 200,
      data: mockPtzState,
      message: 'success',
    }),
  },
  {
    url: '/api/media/ptz/state',
    method: 'post',
    response: ({ body }: any) => {
      mockPtzState = { ...mockPtzState, ...body };
      return {
        code: 200,
        data: mockPtzState,
        message: 'success',
      };
    },
  },
  {
    url: '/api/media/ptz/move',
    method: 'post',
    response: () => ({
      code: 200,
      data: null,
      message: 'success',
    }),
  },
] as MockMethod[];
