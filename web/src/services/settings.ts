import request from '@/services/request';
import { getItem } from '@/utils/storage';

// ==================== Audio Types & API ====================

export interface AudioDeviceInfo {
  name: string;
  description: string;
}

export interface AudioStatus {
  capturing: boolean;
  playing: boolean;
  device: string;
  sample_rate: number;
  channels: number;
  codec: string;
  volume: number;
  mute: boolean;
  /** Whether the device speaker (talk) path is enabled. Gates push-to-talk. */
  playback_enabled: boolean;
}

export interface AudioConfig {
  device?: string;
  sample_rate?: number;
  channels?: number;
  codec?: string;
  bitrate?: number;
  volume?: number;
  mute?: boolean;
  /** Enable/disable the device speaker (talk) path. Persisted server-side. */
  playback_enabled?: boolean;
}

export const fetchAudioStatus = async (): Promise<AudioStatus> => {
  const response = await request.get('/api/v1/audio/status');
  return response.data;
};

export const fetchCaptureDevices = async (): Promise<AudioDeviceInfo[]> => {
  const response = await request.get('/api/v1/audio/capture-devices');
  return response.data.devices;
};

export const fetchPlaybackDevices = async (): Promise<AudioDeviceInfo[]> => {
  const response = await request.get('/api/v1/audio/playback-devices');
  return response.data.devices;
};

export const startAudioCapture = async (config: AudioConfig): Promise<void> => {
  await request.post('/api/v1/audio/capture/start', config);
};

export const stopAudioCapture = async (): Promise<void> => {
  await request.post('/api/v1/audio/capture/stop');
};

export const setAudioConfig = async (config: AudioConfig): Promise<void> => {
  await request.put('/api/v1/audio/config', config);
};

/**
 * Build the WebSocket URL for the push-to-talk stream (browser mic → device
 * speaker). Mirrors how the player builds its /audio/stream URL: same origin,
 * same stored auth token passed as a query param.
 */
export const audioTalkStreamUrl = (): string => {
  const baseUrl = window.location.origin.replace(/^http/, 'ws');
  let token = getItem<string>('token') || '';
  if (token.startsWith('Bearer ')) {
    token = token.substring(7);
  }
  return `${baseUrl}/api/v1/audio/talk?token=${encodeURIComponent(token)}`;
};

// ==================== Image Settings Types & API ====================

export interface ISPConfig {
  manual_mode: boolean;
  brightness: number;
  contrast: number;
  saturation: number;
  sharpness: number;
  auto_exposure: boolean;
  backlight: number;
  exposure_time_us: number;
  gain: number;
  wdr_value: number;
  noise_reduction: number;
  powerline_freq: 0 | 1 | 2;
  awb_index: number;
}

export type PartialISPConfig = Partial<ISPConfig>;

export interface TransformConfig {
  rotation: 0 | 1 | 2 | 3;
  flip: 0 | 1 | 2 | 3;
  dewarp: boolean;
  grayscale: boolean;
  dis: boolean;
  eis: boolean;
}

export type PartialTransformConfig = Partial<TransformConfig>;

export const fetchISPConfig = async (): Promise<ISPConfig> => {
  const response = await request.get('/api/v1/media/image');
  return response.data;
};

export const updateISPConfig = async (
  config: PartialISPConfig
): Promise<void> => {
  await request.put('/api/v1/media/image', config);
};

export const fetchTransformConfig = async (): Promise<TransformConfig> => {
  const response = await request.get('/api/v1/media/transform');
  return response.data;
};

export const updateTransformConfig = async (
  config: PartialTransformConfig
): Promise<void> => {
  window.dispatchEvent(
    new CustomEvent('aipc:media-transform-updating', {
      detail: { active: true },
    })
  );

  try {
    await request.put('/api/v1/media/transform', config);
  } finally {
    window.dispatchEvent(
      new CustomEvent('aipc:media-transform-updating', {
        detail: { active: false },
      })
    );
  }
};

// ==================== AI ISP Profile API ====================
// The device exposes a medialib profile allowlist (AI_ISP_Gen{1,2,3}_Basic).
// Switching restarts the GStreamer pipeline for ~interrupt_ms; the player must
// tear down + reconnect after that window (see player.tsx).

export interface ProfileListResponse {
  profiles: string[];
  current_profile: string;
}

export interface ProfileSwitchResult {
  success: boolean;
  message: string;
  interrupt_ms: number;
}

export const fetchProfiles = async (): Promise<ProfileListResponse> => {
  const response = await request.get('/api/v1/media/profiles');
  return response.data;
};

export const fetchCurrentProfile = async (): Promise<string> => {
  const response = await request.get('/api/v1/media/profile');
  return response.data.profile_name;
};

export const switchProfile = async (
  profileName: string
): Promise<ProfileSwitchResult> => {
  const response = await request.post('/api/v1/media/profile/switch', {
    profile_name: profileName,
  });
  return response.data;
};

// ==================== Network Types & API ====================

// Types
export interface NetworkConfig {
  interface: string;
  mode: 'dhcp' | 'static';
  ip_address: string;
  subnet_mask: string;
  gateway: string;
  dns1: string;
  dns2: string;
  mac_address: string;
}

export interface NetworkInterface {
  name: string;
  ip_address: string;
  subnet_mask: string;
  mac_address: string;
  status: 'up' | 'down';
  is_default: boolean;
  gateway: string;
  dns: string[];
}

// API functions
export const fetchNetworkConfig = async (): Promise<NetworkConfig> => {
  const response = await request.get('/api/v1/network/config');
  return response.data;
};

export const updateNetworkConfig = async (
  data: Partial<NetworkConfig>
): Promise<NetworkConfig> => {
  const response = await request.post('/api/v1/network/config', data);
  return response.data;
};

export const fetchNetworkInterfaces = async (): Promise<NetworkInterface[]> => {
  const response = await request.get('/api/v1/network/interfaces');
  return response.data.interfaces;
};

// Device Info Types
export interface FactoryInfo {
  available: boolean;
  source?: string;
  serial_number?: string;
  mac_address?: string;
  product_number?: string;
  batch?: string;
  hardware_revision?: string;
  error?: string;
}

export interface DeviceInfo {
  device_name: string;
  model: string;
  serial_number: string;
  os_version?: string;
  os_build_time?: string;
  distro?: string;
  kernel_version?: string;
  firmware_version: string;
  build_date: string;
  git_commit: string;
  hardware_version: string;
  soc: {
    vendor: string;
    model: string;
    core?: string;
    npu?: string;
  };
  cpu: {
    model: string;
    cores: number;
    frequency_mhz: number;
  };
  memory: {
    total_gb: number;
    used_gb: number;
    used_percent: number;
  };
  camera_module: {
    model: string;
    i2c_address: string;
    pixel_format: number;
  };
  mac_address: string;
  ip_address: string;
  uptime: number;
  uptime_formatted: string;
  install_prefix: string;
  factory?: FactoryInfo;
  ota: {
    update_available: boolean;
    current_version: string;
    latest_version?: string;
    changelog?: string;
  };
}

// Device Info API
export const fetchDeviceInfo = async (): Promise<DeviceInfo> => {
  const response = await request.get('/api/v1/device-info');
  return response.data;
};

export const updateDeviceName = async (
  deviceName: string
): Promise<{ device_name: string; message: string }> => {
  const response = await request.put('/api/v1/device-info', {
    device_name: deviceName,
  });
  return response.data;
};

export const fetchFactoryInfo = async (): Promise<FactoryInfo> => {
  const response = await request.get('/api/v1/device-info/factory');
  return response.data;
};

export const updateFactoryField = async (
  field: string,
  value: string
): Promise<{ field: string; value: string; factory: FactoryInfo; message: string }> => {
  const response = await request.post('/api/v1/device-info/factory', {
    field,
    value,
  });
  return response.data;
};

export const changePassword = async (_data: {
  oldPassword?: string;
  newPassword: string;
}) => {
  // Mock API call
  await new Promise(resolve => {
    setTimeout(resolve, 1000);
  });
  // Simulate success
  return { success: true };
};

export const factoryReset = async () => {
  // Mock API call
  await new Promise(resolve => {
    setTimeout(resolve, 1500);
  });
  // Simulate success
  return { success: true };
};

export const rebootSystem = async () => {
  // Mock API call
  await new Promise(resolve => {
    setTimeout(resolve, 800);
  });
  // Simulate success
  return { success: true };
};

// ==================== Cellular (EM05) Types & API ====================

export type CellularCarrierProfile = 'generic' | 'verizon';
export type CellularIpType = 'ipv4' | 'ipv6' | 'ipv4v6';
export type CellularAuth = 'none' | 'pap' | 'chap';
export type CellularUsbnet = 'qmi' | 'ecm' | 'mbim';
/** RAT preference via AT+QCFG="nwscanmode". EM05-G has no GSM. */
export type CellularNetworkMode = 'auto' | 'lte' | 'wcdma';

export interface CellularConfig {
  /** Module GPIO power (master switch). */
  enabled: boolean;
  /** Cellular PDP / auto-dial (requires module power). */
  data_enabled: boolean;
  /** Quectel GNSS engine (requires module power). */
  gnss_enabled: boolean;
  apn: string;
  user: string;
  password: string;
  auth: CellularAuth;
  ip_type: CellularIpType;
  pdp_cid: number;
  carrier_profile: CellularCarrierProfile;
  /** Quectel usbnet: qmi | mbim | ecm. */
  usbnet: CellularUsbnet;
  /** Preferred radio access: auto | lte | wcdma. */
  network_mode: CellularNetworkMode;
  /** Allow data roaming (AT+QCFG roamservice). */
  roaming: boolean;
  /** SIM PIN when the card requires unlock (optional). */
  pin: string;
  plmn: string;
  lte_bands: string;
  mtu: number;
  mbn: string;
}

export interface CellularMBNEntry {
  index: number;
  selected: boolean;
  activated: boolean;
  name: string;
}

export type CellularCapability = 'pending' | 'supported' | 'unsupported';

export interface CellularStatus {
  module_present: boolean;
  module?: string;
  platform?: string;
  sim_ready: boolean;
  operator: string;
  network?: string;
  imei?: string;
  iccid?: string;
  imsi?: string;
  msisdn?: string;
  firmware?: string;
  signal_rsrp: number | null;
  signal_rsrq?: number | null;
  reg_state: string;
  connected: boolean;
  iface: string;
  ipv4: string;
  ipv6: string;
  dialer_ready?: boolean;
  supported?: boolean;
  capability?: CellularCapability;
  message: string;
  stub: boolean;
}

const CELLULAR_STORAGE_KEY = 'aipc.cellular.config';

/** Factory defaults — keep in sync with handlers.defaultCellularConfig(). */
const DEFAULT_CELLULAR_CONFIG: CellularConfig = {
  enabled: true,
  data_enabled: true,
  gnss_enabled: true,
  apn: '',
  user: '',
  password: '',
  auth: 'none',
  ip_type: 'ipv4v6',
  pdp_cid: 1,
  carrier_profile: 'generic',
  usbnet: 'mbim',
  network_mode: 'auto',
  roaming: false,
  pin: '',
  plmn: '',
  lte_bands: '',
  mtu: 0,
  mbn: '',
};

function readLocalCellularConfig(): CellularConfig {
  try {
    const raw = getItem(CELLULAR_STORAGE_KEY);
    if (!raw) return { ...DEFAULT_CELLULAR_CONFIG };
    const parsed = typeof raw === 'string' ? JSON.parse(raw) : raw;
    return { ...DEFAULT_CELLULAR_CONFIG, ...parsed };
  } catch {
    return { ...DEFAULT_CELLULAR_CONFIG };
  }
}

function writeLocalCellularConfig(cfg: CellularConfig) {
  try {
    localStorage.setItem(CELLULAR_STORAGE_KEY, JSON.stringify(cfg));
  } catch {
    // ignore quota / private mode
  }
}

const STUB_CELLULAR_STATUS: CellularStatus = {
  module_present: false,
  sim_ready: false,
  operator: '',
  network: '',
  imei: '',
  iccid: '',
  imsi: '',
  msisdn: '',
  firmware: '',
  signal_rsrp: null,
  signal_rsrq: null,
  reg_state: 'unknown',
  connected: false,
  iface: '',
  ipv4: '',
  ipv6: '',
  message:
    'Cellular backend is not ready yet. Kernel WWAN drivers / dialer service pending.',
  stub: true,
};

export const fetchCellularStatus = async (): Promise<CellularStatus> => {
  try {
    const response = await request.get('/api/v1/cellular/status');
    return { ...STUB_CELLULAR_STATUS, ...response.data, stub: false };
  } catch {
    return { ...STUB_CELLULAR_STATUS };
  }
};

export type CellularGnssFix = 'none' | '2d' | '3d';

export interface CellularGnssStatus {
  enabled: boolean;
  active: boolean;
  fix: CellularGnssFix;
  latitude: number | null;
  longitude: number | null;
  altitude: number | null;
  hdop: number | null;
  satellites: number | null;
  utc?: string;
  message: string;
}

const STUB_CELLULAR_GNSS_STATUS: CellularGnssStatus = {
  enabled: false,
  active: false,
  fix: 'none',
  latitude: null,
  longitude: null,
  altitude: null,
  hdop: null,
  satellites: null,
  message: '',
};

export const fetchCellularGnssStatus = async (): Promise<CellularGnssStatus> => {
  try {
    const response = await request.get('/api/v1/cellular/gnss/status');
    const data = (response as { data?: CellularGnssStatus }).data ?? response;
    return { ...STUB_CELLULAR_GNSS_STATUS, ...(data as CellularGnssStatus) };
  } catch {
    return { ...STUB_CELLULAR_GNSS_STATUS };
  }
};

export type CellularCapabilityStatus = {
  capability: CellularCapability;
  module_present: boolean;
};

export const fetchCellularCapability =  async (): Promise<CellularCapabilityStatus> => {
    try {
      const response = await request.get('/api/v1/cellular/capability');
      return {
        capability: 'unsupported',
        module_present: false,
        ...response.data,
      };
    } catch {
      return { capability: 'unsupported', module_present: false };
    }
  };

export const fetchCellularConfig = async (): Promise<CellularConfig> => {
  try {
    const response = await request.get('/api/v1/cellular/config');
    return { ...DEFAULT_CELLULAR_CONFIG, ...response.data };
  } catch {
    return readLocalCellularConfig();
  }
};

export const fetchCellularMBN = async (): Promise<CellularMBNEntry[]> => {
  try {
    const response = await request.get('/api/v1/cellular/mbn');
    const data = (response as { data?: { entries?: CellularMBNEntry[] } }).data ?? response;
    const { entries } = (data as { entries?: CellularMBNEntry[] });
    return Array.isArray(entries) ? entries : [];
  } catch {
    return [];
  }
};

function cellularApiErrorMessage(err: unknown): string | null {
  // request.ts rejects either AxiosError or the bare response object.
  const anyErr = err as {
    response?: { status?: number; data?: { code?: number; error?: { detail?: string }; message?: string } };
    status?: number;
    data?: { code?: number; error?: { detail?: string }; message?: string };
    message?: string;
  };
  const status = anyErr?.response?.status ?? anyErr?.status;
  const body = anyErr?.response?.data ?? anyErr?.data;
  if (status == null && body?.code == null) return null;
  return body?.error?.detail || body?.message || anyErr?.message || `HTTP ${status}`;
}

export const updateCellularConfig = async (
  data: Partial<CellularConfig>
): Promise<CellularConfig> => {
  const next = { ...readLocalCellularConfig(), ...data };
  try {
    const response = await request.put('/api/v1/cellular/config', next);
    // Interceptor returns { code, data, message } on success.
    const payload = (response as { data?: CellularConfig }).data ?? response;
    const saved = { ...DEFAULT_CELLULAR_CONFIG, ...(payload as CellularConfig) };
    writeLocalCellularConfig(saved);
    return saved;
  } catch (err) {
    const detail = cellularApiErrorMessage(err);
    if (detail) {
      throw new Error(detail);
    }
    // True offline / no HTTP response: keep local stub behavior.
    writeLocalCellularConfig(next);
    return next;
  }
};

export function applyCarrierProfile(
  profile: CellularCarrierProfile,
  current: CellularConfig
): CellularConfig {
  if (profile === 'verizon') {
    return {
      ...current,
      carrier_profile: 'verizon',
      apn: current.apn || 'vzwinternet',
      pdp_cid: 3,
      ip_type: 'ipv4v6',
      auth: 'none',
      usbnet: current.usbnet === 'ecm' ? 'ecm' : 'qmi',
    };
  }
  return {
    ...current,
    carrier_profile: 'generic',
    pdp_cid: current.pdp_cid === 3 ? 1 : current.pdp_cid,
  };
}

export interface CellularDebugATResult {
  ok: boolean;
  message: string;
  response: string;
}

export interface CellularDebugPingResult {
  ok: boolean;
  message: string;
  output: string;
  iface: string;
}

export const sendCellularAT = async (
  command: string
): Promise<CellularDebugATResult> => {
  const response = await request.post('/api/v1/cellular/debug/at', { command });
  const data = ((response as { data?: CellularDebugATResult }).data
    ?? response) as Partial<CellularDebugATResult>;
  return {
    ok: Boolean(data.ok),
    message: data.message ?? '',
    response: data.response ?? '',
  };
};

export const sendCellularPing = async (opts: {
  host?: string;
  count?: number;
  iface?: string;
}): Promise<CellularDebugPingResult> => {
  const response = await request.post('/api/v1/cellular/debug/ping', opts);
  const data = ((response as { data?: CellularDebugPingResult }).data
    ?? response) as Partial<CellularDebugPingResult>;
  return {
    ok: Boolean(data.ok),
    message: data.message ?? '',
    output: data.output ?? '',
    iface: data.iface ?? '',
  };
};

export interface CellularQLogStatus {
  ok: boolean;
  available: boolean;
  running: boolean;
  binary: string;
  port: string;
  dm_port: string;
  dir: string;
  files: number;
  bytes: number;
  started_at: string;
  message: string;
}

export interface CellularQLogActionResult {
  ok: boolean;
  message: string;
  running?: boolean;
  port?: string;
  dir?: string;
  binary?: string;
  removed?: number;
}

export const fetchCellularQLogStatus =  async (): Promise<CellularQLogStatus> => {
    const response = await request.get('/api/v1/cellular/debug/qlog');
    const data = ((response as { data?: CellularQLogStatus }).data
      ?? response) as Partial<CellularQLogStatus>;
    return {
      ok: Boolean(data.ok),
      available: Boolean(data.available),
      running: Boolean(data.running),
      binary: data.binary ?? '',
      port: data.port ?? '',
      dm_port: data.dm_port ?? '',
      dir: data.dir ?? '',
      files: Number(data.files ?? 0),
      bytes: Number(data.bytes ?? 0),
      started_at: data.started_at ?? '',
      message: data.message ?? '',
    };
  };

export const startCellularQLog = async (opts?: {
  port?: string;
  max_mb?: number;
  max_files?: number;
}): Promise<CellularQLogActionResult> => {
  const response = await request.post(
    '/api/v1/cellular/debug/qlog/start',
    opts ?? {}
  );
  const data = ((response as { data?: CellularQLogActionResult }).data
    ?? response) as Partial<CellularQLogActionResult>;
  return {
    ok: Boolean(data.ok),
    message: data.message ?? '',
    running: data.running,
    port: data.port,
    dir: data.dir,
    binary: data.binary,
  };
};

export const stopCellularQLog =  async (): Promise<CellularQLogActionResult> => {
    const response = await request.post('/api/v1/cellular/debug/qlog/stop');
    const data = ((response as { data?: CellularQLogActionResult }).data
      ?? response) as Partial<CellularQLogActionResult>;
    return {
      ok: Boolean(data.ok),
      message: data.message ?? '',
      running: data.running,
    };
  };

export const clearCellularQLog =  async (): Promise<CellularQLogActionResult> => {
    const response = await request.post('/api/v1/cellular/debug/qlog/clear');
    const data = ((response as { data?: CellularQLogActionResult }).data
      ?? response) as Partial<CellularQLogActionResult>;
    return {
      ok: Boolean(data.ok),
      message: data.message ?? '',
      removed: data.removed,
    };
  };

export const downloadCellularQLog = async (): Promise<void> => {
  const result = await request.get('/api/v1/cellular/debug/qlog/export', {
    responseType: 'blob',
  });
  const blob = result as unknown as Blob;
  if (!blob || blob.size === 0) {
    throw new Error('Received empty QLog archive');
  }
  const url = window.URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = `cellular-qlog-${new Date().toISOString().slice(0, 10)}.tar.gz`;
  document.body.appendChild(a);
  a.click();
  window.URL.revokeObjectURL(url);
  document.body.removeChild(a);
};

// ==================== Auto Iris (AF0832 aperture) API ====================

export interface AutoIrisStatus {
  enabled: boolean;
  exposure_time_us: number;
  gain: number;
  iris_target: number;
  iris_adc: number;
  state: string;
  reason: string;
}

export const fetchAutoIrisStatus = async (): Promise<AutoIrisStatus> => {
  const response = await request.get('/api/v1/media/auto-iris-status');
  return response.data;
};

export const setAutoIrisEnabled = async (
  enabled: boolean
): Promise<AutoIrisStatus> => {
  const response = await request.put('/api/v1/media/auto-iris', { enabled });
  return response.data;
};
