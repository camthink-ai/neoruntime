import request from '@/services/request';

// ── Types ─────────────────────────────────────────────────────────────

export interface LensAxisLimit {
  min_pos: number;
  max_pos: number;
}

export const MotorState = {
  NoCfg: 0,
  Stopped: 1,
  Running: 2,
  ResetZero: 3,
  Error: 4,
} as const;
export type MotorState = (typeof MotorState)[keyof typeof MotorState];

export interface LensStatus {
  zoom_state: MotorState;
  focus_state: MotorState;
  zoom_rz_done: boolean;
  focus_rz_done: boolean;
  zoom_pos: number;
  focus_pos: number;
  iris_adc: number;
  autofocus_enabled: boolean;
  zoom_limit: LensAxisLimit;
  focus_limit: LensAxisLimit;
}

export interface AutofocusStatus {
  job_id: number;
  operation: 'none' | 'startup' | 'oneshot' | 'zoom_follow';
  state: string;
  progress: number;
  busy: boolean;
  anchor_valid: boolean;
  requested_ratio: number;
  effective_ratio: number;
  zoom_pos: number;
  focus_pos: number;
  best_focus: number;
  metric: number;
  confidence: number;
  reproducibility: number;
  estimated_distance_m: number;
  elapsed_ms: number;
  error_code: number;
  message: string;
}

export interface AutofocusJob {
  accepted: boolean;
  job_id: number;
  message: string;
}

export interface DeviceStatus {
  soc_temp_c: number;
  mcu_temp_c: number;
  light_sensor: number;
  zoom_pos: number;
  focus_pos: number;
  ircut_mode: number | string;
  white_light_level: number;
  ir_led_level: number;
  mcu_version: string;
}

export type IrCutMode = 'day' | 'night';

// ── API ───────────────────────────────────────────────────────────────

export const deviceApi = {
  getStatus: () => request.get('/api/v1/device/status'),

  setLight: (level: number) => request.post('/api/v1/device/light', { level }),

  setIrLed: (level: number) => request.post('/api/v1/device/ir-led', { level }),

  setIrCut: (mode: IrCutMode) => request.post('/api/v1/device/ir-cut', { mode }),

  controlZoom: (speed: number) => request.post('/api/v1/device/zoom', { speed }),

  controlFocus: (speed: number) => request.post('/api/v1/device/focus', { speed }),

  setAutofocus: (enable: boolean) => request.post('/api/v1/device/autofocus', { enable }),

  oneshotAutofocus: () => request.post('/api/v1/device/lens/oneshot-af'),

  startZoomFollow: (ratio: number) => request.post('/api/v1/device/lens/zoom-follow', { ratio }),

  getAutofocusStatus: () => request.get('/api/v1/device/lens/af/status', { silent: true }),

  cancelAutofocus: (jobId = 0) => request.post('/api/v1/device/lens/af/cancel', { job_id: jobId }),

  getLensStatus: () => request.get('/api/v1/device/lens/status'),

  setZoomLevel: (level: number) => request.put('/api/v1/device/lens/zoom-level', { level }),

  setFocusLevel: (level: number) => request.put('/api/v1/device/lens/focus-level', { level }),

  resetLensZero: (zoom: boolean, focus: boolean) => request.post('/api/v1/device/lens/reset-zero', { zoom, focus }),

  setLensLimits: (data: {
    zoom_limit?: LensAxisLimit;
    focus_limit?: LensAxisLimit;
  }) => request.put('/api/v1/device/lens/limits', data),

  controlPTZ: (action: string, params?: Record<string, unknown>) => request.post('/api/v1/device/ptz', { action, ...params }),

  // Environment control
  setFan: (enable: boolean) => request.post('/api/v1/device/fan', { enable }),
  getFan: () => request.get('/api/v1/device/fan'),
  setHeat: (enable: boolean) => request.post('/api/v1/device/heat', { enable }),
  getHeat: () => request.get('/api/v1/device/heat'),
  setRadar: (enable: boolean) => request.post('/api/v1/device/radar', { enable }),
  getRadar: () => request.get('/api/v1/device/radar'),

  // Alarm I/O
  setAlarmOut: (channel: number, enable: boolean) => request.post('/api/v1/device/alarm-out', { channel, enable }),
  getAlarmOutputs: () => request.get('/api/v1/device/alarm-outputs'),
  setWiegand: (channel: number, enable: boolean) => request.post('/api/v1/device/wiegand', { channel, enable }),

  // Capabilities
  getCapabilities: () => request.get('/api/v1/device/capabilities'),
};
