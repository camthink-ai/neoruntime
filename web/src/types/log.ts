export type LogType = 'service' | 'file';
export type LogCategory = 'all' | 'operation' | 'security' | 'alarm' | 'system';
export type LogLevel = 'error' | 'warning' | 'info' | 'debug' | 'fatal';

export interface LogService {
  id: string;
  name: string;
}

export interface LogFile {
  id: string;
  name: string;
  path: string;
  size: number;
  modified_time: string;
  service?: string;
}

export interface LogLine {
  timestamp?: string;
  level?: string;
  message: string;
  raw: string;
}

// New structured log types
export interface LogEntry {
  id: string;
  timestamp: string;
  level: LogLevel;
  category: LogCategory;
  source: string;
  title_key: string;
  title: string;
  message: string;
  raw?: string;
  metadata?: LogMetadata;
}

export interface LogMetadata {
  hostname?: string;
  process?: string;
  process_id?: string;
  boot_id?: string;
  machine_id?: string;
  line_number?: number;
}

export interface LogStatistics {
  today_errors: number;
  today_warnings: number;
  operations: number;
  security_events: number;
  alarm_events: number;
  system_events: number;
  total_entries: number;
}

export interface LogFilterOptions {
  category?: LogCategory;
  level?: LogLevel;
  start_time?: string;
  end_time?: string;
  search?: string;
  limit?: number;
  offset?: number;
}

export interface LogEntriesResponse {
  type: LogType;
  target: string;
  total: number;
  filtered: number;
  entries: LogEntry[];
}

export interface LogFilterParams {
  filterText?: string;
  lines?: number;
  offset?: number;
}

export interface LogStreamOptions {
  type: LogType;
  target: string;
  token?: string;
  onMessage?: (data: string) => void;
  onError?: (error: Event) => void;
  onClose?: () => void;
}
