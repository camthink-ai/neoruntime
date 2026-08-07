/**
 * Event Log Types - User Event Logs (Layer 1)
 * These are user-friendly, persistent event logs stored in the database
 */

export type EventCategory = 'all' | 'operation' | 'system';
export type EventLevel = 'critical' | 'error' | 'warning' | 'info';

/**
 * Event log entry as returned by the API
 */
export interface EventLogEntry {
  id: number;
  event_id: string;
  timestamp: string;
  event_type: string;
  level: EventLevel;
  category: EventCategory;
  source: string;
  module: string;
  user?: string;
  message: string;
  data?: Record<string, unknown>;
  created_at: string;
}

/**
 * Event statistics
 */
export interface EventStatistics {
  today_errors: number;
  today_warnings: number;
  today_critical: number;
  operations: number;
  system_events: number;
  total_entries: number;
}

/**
 * Event filter options
 */
export interface EventFilters {
  category?: EventCategory;
  level?: EventLevel;
  start_time?: string;
  end_time?: string;
  search?: string;
  limit?: number;
  offset?: number;
}

/**
 * Event list response
 */
export interface EventListResponse {
  total: number;
  limit: number;
  offset: number;
  entries: EventLogEntry[];
}

/**
 * Debug log export options
 */
export interface DebugLogExportOptions {
  services?: string[];
  files?: string[];
  lines?: number;
}

/**
 * Debug log service info
 */
export interface DebugLogService {
  id: string;
  name: string;
}

/**
 * Debug log file info
 */
export interface DebugLogFile {
  path: string;
  name: string;
  size: number;
  modified_time: string;
}
