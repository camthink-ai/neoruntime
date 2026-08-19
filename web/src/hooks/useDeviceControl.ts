import { useMutation, useQuery, useQueryClient } from '@tanstack/react-query';
import { deviceApi } from '@/services/api';
import type {
  AutofocusJob,
  AutofocusStatus,
  DeviceStatus,
  IrCutMode,
  LensStatus,
} from '@/services/api/device';

export const useDeviceStatus = () => useQuery<DeviceStatus>({
    queryKey: ['device', 'status'],
    queryFn: async () => {
      const response = await deviceApi.getStatus();
      return (response as any).data as DeviceStatus;
    },
  });

export const useSetLight = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (level: number) => {
      const response = await deviceApi.setLight(level);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useSetIrLed = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (level: number) => {
      const response = await deviceApi.setIrLed(level);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useSetIrCut = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (mode: IrCutMode) => {
      const response = await deviceApi.setIrCut(mode);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useControlZoom = () => useMutation({
    mutationFn: async (speed: number) => {
      const response = await deviceApi.controlZoom(speed);
      return (response as any).data;
    },
  });

export const useControlFocus = () => useMutation({
    mutationFn: async (speed: number) => {
      const response = await deviceApi.controlFocus(speed);
      return (response as any).data;
    },
  });

export const useSetAutofocus = () => useMutation({
    mutationFn: async (enable: boolean) => {
      const response = await deviceApi.setAutofocus(enable);
      return (response as any).data;
    },
  });

export const useOneshotAutofocus = () => useMutation({
    mutationFn: async () => {
      const response = await deviceApi.oneshotAutofocus();
      return (response as any).data as AutofocusJob;
    },
  });

export const useStartZoomFollow = () => useMutation({
    mutationFn: async (ratio: number) => {
      const response = await deviceApi.startZoomFollow(ratio);
      return (response as any).data as AutofocusJob;
    },
  });

export const useAutofocusStatus = () => useQuery<AutofocusStatus>({
    queryKey: ['device', 'lens', 'autofocus'],
    queryFn: async () => {
      const response = await deviceApi.getAutofocusStatus();
      return (response as any).data as AutofocusStatus;
    },
    refetchInterval: 400,
  });

export const useCancelAutofocus = () => useMutation<unknown, Error, number | undefined>({
    mutationFn: async jobId => {
      const response = await deviceApi.cancelAutofocus(jobId ?? 0);
      return (response as any).data;
    },
  });

export const useLensStatus = () => useQuery<LensStatus>({
    queryKey: ['device', 'lens', 'status'],
    queryFn: async () => {
      const response = await deviceApi.getLensStatus();
      return (response as any).data as LensStatus;
    },
  });

export const useSetZoomLevel = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (level: number) => {
      const response = await deviceApi.setZoomLevel(level);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'lens', 'status'] });
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useSetFocusLevel = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (level: number) => {
      const response = await deviceApi.setFocusLevel(level);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'lens', 'status'] });
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useResetLensZero = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async ({ zoom, focus }: { zoom: boolean; focus: boolean }) => {
      const response = await deviceApi.resetLensZero(zoom, focus);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'lens', 'status'] });
      queryClient.invalidateQueries({ queryKey: ['device', 'status'] });
    },
  });
};

export const useSetLensLimits = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (data: {
      zoom_limit?: { min_pos: number; max_pos: number };
      focus_limit?: { min_pos: number; max_pos: number };
    }) => {
      const response = await deviceApi.setLensLimits(data);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({ queryKey: ['device', 'lens', 'status'] });
    },
  });
};

export const useControlPTZ = () => useMutation({
    mutationFn: async (params: {
      action: string;
      direction?: string;
      speed?: number;
      preset_id?: number;
    }) => {
      const response = await deviceApi.controlPTZ(params.action, params);
      return (response as any).data;
    },
  });

// ── Peripheral hooks ────────────────────────────────────────────────

export const useSetFan = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (enable: boolean) => {
      const response = await deviceApi.setFan(enable);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['device', 'peripheral', 'fan'],
      });
    },
  });
};

export const useGetFan = () => useQuery({
    queryKey: ['device', 'peripheral', 'fan'],
    queryFn: async () => {
      const response = await deviceApi.getFan();
      return (response as any).data;
    },
  });

export const useSetHeat = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (enable: boolean) => {
      const response = await deviceApi.setHeat(enable);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['device', 'peripheral', 'heat'],
      });
    },
  });
};

export const useGetHeat = () => useQuery({
    queryKey: ['device', 'peripheral', 'heat'],
    queryFn: async () => {
      const response = await deviceApi.getHeat();
      return (response as any).data;
    },
  });

export const useSetRadar = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async (enable: boolean) => {
      const response = await deviceApi.setRadar(enable);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['device', 'peripheral', 'radar'],
      });
    },
  });
};

export const useGetRadar = () => useQuery({
    queryKey: ['device', 'peripheral', 'radar'],
    queryFn: async () => {
      const response = await deviceApi.getRadar();
      return (response as any).data;
    },
  });

export const useSetAlarmOut = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async ({
      channel,
      enable,
    }: {
      channel: number;
      enable: boolean;
    }) => {
      const response = await deviceApi.setAlarmOut(channel, enable);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['device', 'peripheral', 'alarm-outputs'],
      });
    },
  });
};

export const useSetWiegand = () => {
  const queryClient = useQueryClient();
  return useMutation({
    mutationFn: async ({
      channel,
      enable,
    }: {
      channel: number;
      enable: boolean;
    }) => {
      const response = await deviceApi.setWiegand(channel, enable);
      return (response as any).data;
    },
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['device', 'peripheral', 'alarm-outputs'],
      });
    },
  });
};

export const useAlarmOutputs = () => useQuery({
    queryKey: ['device', 'peripheral', 'alarm-outputs'],
    queryFn: async () => {
      const response = await deviceApi.getAlarmOutputs();
      return (response as any).data;
    },
  });

export interface DeviceCapabilities {
  has_video: boolean;
  has_codec: boolean;
  has_led: boolean;
  has_sensor: boolean;
  has_mcu: boolean;
  has_env_ctrl: boolean;
  has_alarm: boolean;
  has_rs485: boolean;
  has_osd: boolean;
  has_draw: boolean;
  has_audio: boolean;
}

export const useCapabilities = () => useQuery<DeviceCapabilities>({
    queryKey: ['device', 'capabilities'],
    queryFn: async () => {
      const response = await deviceApi.getCapabilities();
      return (response as any).data as DeviceCapabilities;
    },
    staleTime: 60000,
  });
