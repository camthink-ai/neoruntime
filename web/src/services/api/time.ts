import request from '@/services/request';

// Time types - single source of truth
export interface SystemTime {
  current_time: string;
  unix_timestamp: number;
  timezone: string;
  uptime: number;
  uptime_formatted: string;
}

export interface NTPConfig {
  enabled: boolean;
  server: string;
  interval: number; // sync interval in seconds
  synced: boolean;
  last_sync?: string;
  fallback_servers: string[];
}

export interface TimeConfig {
  timezone: string;
  time_format: '12h' | '24h';
  sync_mode: 'ntp' | 'manual' | 'local';
  ntp: NTPConfig;
  auto_sync: boolean;
}

export interface TimezoneData {
  name: string;
  country: string;
  offset: string;
  offset_sec: number;
}

export interface SetTimeRequest {
  datetime: string; // RFC3339 format
}

export interface SetTimezoneRequest {
  timezone: string;
}

export interface SetNTPConfigRequest {
  enabled: boolean;
  server?: string;
  interval?: number;
}

export interface SaveTimeConfigRequest {
  timezone: string;
  time_format: '12h' | '24h';
  sync_mode: 'ntp' | 'manual' | 'local';
  ntp_server?: string;
  ntp_interval?: number;
  manual_datetime?: string;
}

// Standard API response wrapper
interface ApiResponse<T> {
  code: number;
  message: string;
  data: T;
}

// Time API
export const timeApi = {
  // Get current system time
  getSystemTime: async (): Promise<SystemTime> => {
    const response = (await request.get(
      '/api/v1/system/time'
    )) as ApiResponse<SystemTime>;
    return response.data;
  },

  // Get time configuration
  getTimeConfig: async (): Promise<TimeConfig> => {
    const response = (await request.get(
      '/api/v1/system/time/config'
    )) as ApiResponse<TimeConfig>;
    return response.data;
  },

  // Get available timezones
  getTimezones: async (): Promise<TimezoneData[]> => {
    const response = (await request.get(
      '/api/v1/system/time/timezones'
    )) as ApiResponse<TimezoneData[]>;
    return response.data;
  },

  // Set system time manually
  setSystemTime: async (data: SetTimeRequest): Promise<void> => {
    await request.post('/api/v1/system/time/set', data);
  },

  // Set timezone
  setTimezone: async (data: SetTimezoneRequest): Promise<void> => {
    await request.put('/api/v1/system/time/timezone', data);
  },

  // Set NTP configuration
  setNTPConfig: async (data: SetNTPConfigRequest): Promise<void> => {
    await request.put('/api/v1/system/time/ntp', data);
  },

  // Trigger manual NTP sync
  syncNTP: async (): Promise<void> => {
    await request.post('/api/v1/system/time/ntp/sync');
  },

  // Save full time configuration
  saveTimeConfig: async (data: SaveTimeConfigRequest): Promise<void> => {
    await request.put('/api/v1/system/time/config', data);
  },

  // Sync time from browser/client
  syncFromClient: async (
    clientUnixMs: number
  ): Promise<{ synced: boolean; diff_seconds: number; message: string }> => {
    const response = (await request.post(
      '/api/v1/system/time/sync-from-client',
      {
        client_timestamp: Math.floor(clientUnixMs / 1000),
      }
    )) as ApiResponse<{
      synced: boolean;
      diff_seconds: number;
      message: string;
    }>;
    return response.data;
  },
};

export default timeApi;
