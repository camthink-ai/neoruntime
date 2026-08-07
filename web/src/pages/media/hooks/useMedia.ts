import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import {
  disableStream,
  enableStream,
  getPtzState,
  reconfigureEncoder,
  setRtspEnabled,
  updateAiOverlay,
  updateEncoderConfig,
  updateOsdConfig,
} from '@/services/media';
import { updateISPConfig, type PartialISPConfig } from '@/services/settings';
import type { PtzState } from '@/types/media';
import {
  useControlFocus,
  useControlZoom,
  useSetAutofocus,
} from '@/hooks/useDeviceControl';
import { refreshMediaStatusUntilStreamActive } from '../utils/mediaStatusRecovery';
import { toast } from 'sonner';

// Re-exported from the shared hook so the media page keeps importing from here
// unchanged. The devices page imports `useMediaStatus` directly from
// `@/hooks/useMediaStatus`.
export { useMediaStatus, type MediaStreamStatus } from '@/hooks/useMediaStatus';

/**
 * 实时调节 ISP 图像质量
 * 通过 PUT /api/v1/media/image
 */
export const useUpdateImageConfig = () => useMutation({
    mutationFn: (data: PartialISPConfig) => updateISPConfig(data),
    onSuccess: () => {
      // 不在此处 toast 提示，因为是滑动条频繁调用
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || '图像调节失败'
      );
    },
  });

/**
 * Hot reload: Update encoder config (bitrate, fps, gop) without restart
 * 通过 PUT /api/v1/media/encoder
 */
export const useUpdateEncoderConfig = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (data: {
      stream_name: string;
      bitrate_bps?: number;
      framerate?: number;
      gop?: number;
    }) => updateEncoderConfig(data),
    onSuccess: async (_response, variables) => {
      await refreshMediaStatusUntilStreamActive(
        queryClient,
        variables.stream_name
      );
      // A bitrate override can reset the encoder's reference-frame chain.
      // Reconnect the browser decoder so it starts from the clean IDR requested
      // by the backend instead of retaining corrupted transitional frames.
      if (
        variables.bitrate_bps !== undefined
        && typeof window !== 'undefined'
      ) {
        window.dispatchEvent(new CustomEvent('player-reload'));
      }
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || '编码器配置失败'
      );
    },
  });
};

/**
 * Hot reload: Toggle RTSP server without restart
 * 通过 PUT /api/v1/media/rtsp
 */
export const useSetRtspEnabled = () => useMutation({
    mutationFn: (enabled: boolean) => setRtspEnabled(enabled),
    onSuccess: (_resp, enabled) => {
      toast.success(enabled ? 'RTSP 已启用' : 'RTSP 已停止');
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || 'RTSP 切换失败'
      );
    },
  });

/**
 * Hot reload: Update AI overlay config without restart
 * 通过 PUT /api/v1/media/ai-overlay
 */
export const useUpdateAiOverlay = () => useMutation({
    mutationFn: (data: {
      enabled: boolean;
      show_label?: boolean;
      show_confidence?: boolean;
      line_thickness?: number;
    }) => updateAiOverlay(data),
    onSuccess: () => {
      toast.success('AI 叠加配置已生效');
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || 'AI 叠加配置失败'
      );
    },
  });

/**
 * Hot reload: Update OSD config without restart
 * 通过 PUT /api/v1/media/osd
 */
export const useUpdateOsdConfig = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (data: {
      streams: Array<{
        stream_name: string;
        text_overlays?: Array<{
          id: string;
          text?: string;
          x: number;
          y: number;
          font_size?: number;
          text_color?: number;
          enabled: boolean;
        }>;
        datetime_overlays?: Array<{
          id: string;
          x: number;
          y: number;
          format?: string;
          font_size?: number;
          text_color?: number;
          enabled: boolean;
        }>;
      }>;
    }) => updateOsdConfig(data),
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['mediaStatus'] });
      toast.success('OSD 配置已生效');
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || 'OSD 配置失败'
      );
    },
  });
};

/**
 * Enable a stream via POST /api/v1/media/streams/:name/enable
 * Main stream cannot be disabled, so enabling main is a no-op.
 */
export const useEnableStream = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (name: string) => enableStream(name),
    onSuccess: (_, name) => {
      queryClient.setQueryData(['mediaStatus'], (old: any) => {
        const streams = old?.streams;
        if (!Array.isArray(streams)) return old;
        return {
          ...old,
          streams: streams.map((stream: any) => {
            if (stream.stream_id !== name) return stream;
            return { ...stream, status: 'active', has_encoder: true };
          }),
        };
      });
      queryClient.invalidateQueries({ queryKey: ['mediaStatus'] });
    },
    onError: (error: any) => {
      toast.error(
        error?.response?.data?.message
          || error?.message
          || 'Failed to enable stream'
      );
    },
  });
};

/**
 * Disable a stream via DELETE /api/v1/media/streams/:name/disable
 * Main stream cannot be disabled — mutation rejects it.
 */
export const useDisableStream = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (name: string) => {
      if (name === 'main') throw new Error('Main stream cannot be disabled');
      return disableStream(name);
    },
    onSuccess: (_, name) => {
      queryClient.setQueryData(['mediaStatus'], (old: any) => {
        const streams = old?.streams;
        if (!Array.isArray(streams)) return old;
        return {
          ...old,
          streams: streams.map((stream: any) => {
            if (stream.stream_id !== name) return stream;
            return { ...stream, status: 'stopped', has_encoder: false };
          }),
        };
      });
      queryClient.invalidateQueries({ queryKey: ['mediaStatus'] });
    },
    onError: (error: any) => {
      toast.error(
        error?.response?.data?.message
          || error?.message
          || 'Failed to disable stream'
      );
    },
  });
};

/**
 * Full encoder reconfiguration (brief restart required, ~100ms)
 * Use this for resolution/codec changes
 * 通过 PUT /api/v1/media/encoder/reconfig
 */
export const useReconfigureEncoder = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: (data: {
      stream_name: string;
      width?: number;
      height?: number;
      codec?: 'h264' | 'h265';
      bitrate_bps?: number;
      fps?: number;
      gop?: number;
    }) => reconfigureEncoder(data),
    onSuccess: async (_response, variables) => {
      await refreshMediaStatusUntilStreamActive(
        queryClient,
        variables.stream_name
      );
      // Resolution/codec/fps changed → stream identity changed. Force the player
      // to reconnect so it fetches fresh SPS/PPS and waits for a clean IDR,
      // instead of decoding transitional frames against the old reference
      // (花屏/黑屏). Backend already invalidated the SPS cache, reset the UDS
      // readLoop, and will force an IDR on the new WS connection (AddClient).
      if (typeof window !== 'undefined') {
        window.dispatchEvent(new CustomEvent('player-reload'));
      }
    },
    onError: (err: any) => {
      toast.error(
        err?.response?.data?.message || err?.message || '编码器重配置失败'
      );
    },
  });
};

// ========== PTZ hooks (unchanged, using real device-control API) ==========

export const usePtzState = () => useQuery<PtzState>({
    queryKey: ['ptzState'],
    queryFn: async () => getPtzState(),
    initialData: { zoom: 2.4, focus: 50, mode: 'auto' } as PtzState,
  });

export const useUpdatePtzState = () => {
  const queryClient = useQueryClient();
  const controlZoom = useControlZoom();
  const controlFocus = useControlFocus();
  const setAutofocus = useSetAutofocus();

  return useMutation({
    mutationFn: async (data: Partial<PtzState>) => {
      if (data.zoom !== undefined) {
        await controlZoom.mutateAsync(data.zoom);
      }
      if (data.focus !== undefined) {
        await controlFocus.mutateAsync(data.focus);
      }
      if (data.mode !== undefined) {
        await setAutofocus.mutateAsync(data.mode === 'auto');
      }
      return data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['ptzState'] });
    },
  });
};
