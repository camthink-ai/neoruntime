import request, { http } from '@/services/request';

// Backup & migration endpoints. All six live under the authenticated /api/v1
// group, so the token header is injected by services/request.ts automatically.
// Exports are streamed (tar.gz / json) and arrive as Blobs; imports are either
// a JSON body (media config) or a multipart upload (bundle / clone).
const MEDIA_CONFIG_EXPORT = '/api/v1/media/config/export';
const MEDIA_CONFIG_IMPORT = '/api/v1/media/config/import';
const MEDIA_BUNDLE_EXPORT = '/api/v1/media/config/bundle';
const MEDIA_BUNDLE_IMPORT = '/api/v1/media/config/import-bundle';
const CLONE_EXPORT = '/api/v1/system/clone/export';
const CLONE_IMPORT = '/api/v1/system/clone/import';

/**
 * Trigger a browser download for a Blob under the given filename.
 * Extracted from the debug-logs export pattern (services/api/event_logs.ts)
 * so every backup export reuses one canonical helper instead of each page
 * re-implementing createObjectURL + anchor click + revoke.
 */
export function downloadBlob(blob: Blob, filename: string): void {
  if (!blob || blob.size === 0) {
    throw new Error('Received empty file');
  }
  const url = window.URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  window.URL.revokeObjectURL(url);
  document.body.removeChild(a);
}

/** UTC YYYYMMDD-HHMMSS stamp for export filenames (sortable, no colons). */
function exportTimestamp(): string {
  const d = new Date();
  const pad = (n: number) => String(n).padStart(2, '0');
  return (
    `${d.getUTCFullYear()}${pad(d.getUTCMonth() + 1)}${pad(d.getUTCDate())}`
    + `-${pad(d.getUTCHours())}${pad(d.getUTCMinutes())}${pad(d.getUTCSeconds())}`
  );
}

/**
 * Fetch a streamed export as a Blob and save it to disk. The response
 * interceptor (services/request.ts) returns Blob responses raw, so the awaited
 * value is the Blob itself (cast like downloadDebugLogs does).
 */
async function fetchExport(url: string, filename: string): Promise<void> {
  const result = await request.get(url, { responseType: 'blob' });
  downloadBlob(result as unknown as Blob, filename);
}

export const backupApi = {
  /** Export the 8-dimension media config as JSON (config only, no image bytes). */
  exportMediaConfig: () => fetchExport(MEDIA_CONFIG_EXPORT, `media-config_${exportTimestamp()}.json`),

  /** Apply a previously exported media-config JSON envelope. */
  importMediaConfig: (envelope: unknown) => http.post(MEDIA_CONFIG_IMPORT, envelope),

  /** Export the media-config bundle (config + OSD overlay images) as tar.gz. */
  exportMediaBundle: () => fetchExport(
      MEDIA_BUNDLE_EXPORT,
      `media-bundle_${exportTimestamp()}.tar.gz`
    ),

  /** Apply a media-config bundle (.tar.gz) via multipart upload. */
  importMediaBundle: (file: File) => {
    const fd = new FormData();
    fd.append('file', file);
    return http.post(MEDIA_BUNDLE_IMPORT, fd);
  },

  /** Export the full device clone (config + 4 state tables) as tar.gz. */
  exportClone: () => fetchExport(CLONE_EXPORT, `device-clone_${exportTimestamp()}.tar.gz`),

  /** Apply a device clone (.tar.gz) via multipart upload. */
  importClone: (file: File) => {
    const fd = new FormData();
    fd.append('file', file);
    return http.post(CLONE_IMPORT, fd);
  },
};
