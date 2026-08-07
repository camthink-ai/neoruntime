import request from '@/services/request';
import type {
  EventStatistics,
  EventFilters,
  EventListResponse,
  DebugLogExportOptions,
  DebugLogService,
  DebugLogFile,
} from '@/types/event_log';

/**
 * User Event Logs API (Layer 1: User-friendly persistent logs)
 */
export const eventLogsApi = {
  /**
   * Get event message templates (single source of truth from backend)
   */
  getTemplates: () => request.get<{
      templates: Record<
        string,
        {
          en: string;
          zh: string;
          module: string;
          level: string;
          params: string[];
        }
      >;
    }>('/api/v1/event-logs/templates'),

  /**
   * Get event logs list with optional filters
   */
  list: (filters?: EventFilters) => request.get<EventListResponse>('/api/v1/event-logs', {
      params: filters,
    }),

  /**
   * Get event statistics
   */
  getStatistics: () => request.get<EventStatistics>('/api/v1/event-logs/statistics'),

  /**
   * Create a new event log entry (mainly for testing/manual logging)
   */
  create: (data: {
    event_type: string;
    level?: string;
    category?: string;
    source: string;
    user?: string;
    message: string;
    data?: Record<string, unknown>;
  }) => request.post('/api/v1/event-logs', data),

  /**
   * Cleanup old event logs
   */
  cleanup: (days: number) => request.delete('/api/v1/event-logs', { data: { days } }),
};

/**
 * Developer Debug Logs API (Layer 2: Export only)
 */
export const debugLogsApi = {
  /**
   * Export debug logs as tar.gz
   * Returns a Blob that should be downloaded
   */
  export: (options: DebugLogExportOptions = {}) => request.post<Blob>('/api/v1/debug-logs/export', options, {
      responseType: 'blob',
    }),

  /**
   * Get available debug log services
   */
  getServices: () => request.get<{ services: DebugLogService[] }>('/api/v1/debug-logs/services'),

  /**
   * Get available debug log files
   */
  getFiles: () => request.get<{ files: DebugLogFile[] }>('/api/v1/debug-logs/files'),
};

/**
 * Helper function to download exported debug logs
 */
export const downloadDebugLogs = async (
  options: DebugLogExportOptions = {}
): Promise<void> => {
  try {
    const result = await debugLogsApi.export(options);
    // The response interceptor returns data directly for blobs
    const blob = result as unknown as Blob;

    if (!blob || blob.size === 0) {
      throw new Error('Received empty file');
    }

    const url = window.URL.createObjectURL(blob);
    const a = document.createElement('a');
    a.href = url;
    a.download = `debug-logs-${new Date().toISOString().slice(0, 10)}.tar.gz`;
    document.body.appendChild(a);
    a.click();
    window.URL.revokeObjectURL(url);
    document.body.removeChild(a);
  } catch (error) {
    console.error('Export failed:', error);
    throw error;
  }
};
