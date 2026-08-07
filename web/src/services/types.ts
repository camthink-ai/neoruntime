export interface AppTemplateTag {
  id: number;
  key: string;
  name: string;
  name_zh: string;
  sort: number;
  created_at: string;
  updated_at: string;
}

export interface AppVersion {
  id: number;
  app_id: number;
  version: string;
  image: string;
  default_config: string;
  changelog: string;
  status: string;
  release_date: string;
  created_at: string;
  updated_at: string;
}

export interface AppTemplate {
  id: number | string;
  key?: string;
  name: string;
  name_zh?: string;
  icon?: string;
  short_desc?: string;
  short_desc_zh?: string;
  description?: string;
  category?: string;
  author?: string;
  website?: string;
  document?: string;
  architectures?: string;
  min_memory?: number;
  gpu_required?: boolean;
  soc_support?: string;
  source?: string;
  status?: string;
  featured?: boolean;
  sort_order?: number;
  downloads?: number;
  rating?: number;
  created_at?: string;
  updated_at?: string;
  tags?: AppTemplateTag[];
  isInstalled?: boolean;
  cpuUsage?: string;
  iconBgColor?: string;
  iconColor?: string;
  version?: string;
  state?: string;
  manifest_path?: string;
  image_path?: string;
  // Container runtime info
  container_id?: string;
  cpu_percent?: number;
  memory_usage?: number;
  uptime?: number;
  // App timestamps (Unix timestamps in seconds)
  installed_at?: number;
  started_at?: number;
  stopped_at?: number;
  // Permissions from manifest
  permissions?: AppPermissions;
  // SDK-registered web path
  web_url?: string;
}

export interface AppPermissions {
  video?: string[];
  inference?: {
    models?: string[];
    max_qps?: number;
    max_concurrent?: number;
    allow_register_model?: boolean;
  };
  events?: {
    publish?: string[];
    subscribe?: string[];
  };
  device?: {
    light?: boolean;
    ir_cut?: boolean;
    ptz?: boolean;
    lens?: boolean;
  };
  network?: {
    mode?: string;
    outbound?: string[];
    inbound?: number[];
  };
}

export interface AppDetailResponse {
  app: AppTemplate;
  tags: AppTemplateTag[];
  versions: AppVersion[];
}

export interface InstalledApp {
  id: string;
  name: string;
  version: string;
  status: 'running' | 'stopped' | 'paused';
  image: string;
  cpuUsage: number;
  memoryUsage: number;
  memoryUsageMB: number;
  memoryTotalGB: number;
  uptime: number;
}

export interface AppDashboardStats {
  totalApps: number;
  runningApps: number;
  stoppedApps: number;
  totalCpu: number;
  totalMemory: number;
}

// Dashboard Types
export interface SystemStats {
  cpu: {
    usage: number;
    cores: number;
  };
  npu: {
    usage: number;
  };
  memory: {
    usageGB: number;
    totalGB: number;
    usagePercent: number;
  };
  storage: {
    usageGB: number;
    totalGB: number;
    usagePercent: number;
    type: string;
    mountpoint?: string;
  };
}

export interface Activity {
  id: string;
  type: 'app_started' | 'app_stopped' | 'system_update' | 'error';
  message: string;
  timestamp: string;
}

export interface SystemInfo {
  device_name: string;
  model: string;
  firmware_version: string;
  build_date: string;
  mac_address: string;
  ip_address: string;
}

// Platform Status Types
export interface PlatformStats {
  apps: {
    total: number;
    running: number;
    stopped: number;
    list: any[]; // 完整的应用列表
  };
  containers: {
    total: number;
    running: number;
    stopped: number;
    list: any[];
  };
  models: {
    total: number;
    loaded: number;
    list: any[]; // 完整的模型列表
  };
  cameras: {
    total: number;
    online: number;
    offline: number;
  };
}

// Network Stats Types
export interface NetworkStats {
  rx_bytes: number; // 接收字节
  tx_bytes: number; // 发送字节
  rx_rate: number; // 接收速率 (bytes/s)
  tx_rate: number; // 发送速率 (bytes/s)
}

// Monitor Types
export interface DiskPartition {
  device: string;
  mountpoint: string;
  fstype: string;
  total: number;
  used: number;
  free: number;
  usage_percent: number;
  is_system?: boolean;
  is_removable?: boolean;
  is_protected?: boolean;
  mountpoint_label?: string;
  label?: string;
}

export interface DiskInfo {
  partitions: DiskPartition[];
}

export type StorageDeviceType = 'internal' | 'sd_card' | 'usb';

export interface StorageDevice {
  id: string;
  type: StorageDeviceType;
  label: string;
  totalBytes: number;
  usedBytes: number;
  freeBytes: number;
  usagePercent: number;
  partitions: DiskPartition[];
  canFormat: boolean;
  canUnmount: boolean;
  hasUnmountedPartitions: boolean;
}

// Wizard Configuration Types
export interface WizardConfig {
  metadata: {
    id: string;
    name: string;
    version: string;
    description: string;
  };
  image: string;
  image_path?: string;
  resources?: {
    cpu?: string;
    memory?: string;
  };
  permissions?: {
    video?: string[];
    inference?: {
      models?: string[];
      max_qps?: number;
      max_concurrent?: number;
      allow_register_model?: boolean;
    };
    events?: {
      publish?: string[];
      subscribe?: string[];
    };
    device?: {
      light?: boolean;
      ir_cut?: boolean;
      ptz?: boolean;
      lens?: boolean;
    };
    network?: {
      mode?: string;
      inbound?: number[];
    };
  };
  env?: Array<{ name: string; value: string }>;
  volumes?: Array<{ host: string; container: string; readonly?: boolean }>;
  autostart?: boolean;
  restart_policy?: string;
}
