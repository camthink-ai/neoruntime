import request from './request';
import { getItem } from '@/utils/storage';
import type { PtzState, PtzMoveRequest } from '@/types/media';

// Media Status API
export const getMediaStatus = () => request.get('/api/v1/media/status');

// Encoder config hot reload API (no restart required)
export const updateEncoderConfig = (data: {
  stream_name: string;
  bitrate_bps?: number;
  framerate?: number;
  gop?: number;
}) => request.put('/api/v1/media/encoder', data, { silent: true });

// Stream enable/disable API
export const enableStream = (name: string) => request.post(`/api/v1/media/streams/${name}/enable`, undefined, {
    silent: true,
  });
export const disableStream = (name: string) => request.delete(`/api/v1/media/streams/${name}/disable`, { silent: true });

// RTSP toggle hot reload API (no restart required)
export const setRtspEnabled = (enabled: boolean) => request.put('/api/v1/media/rtsp', { enabled }, { silent: true });

// AI overlay config hot reload API (no restart required)
export const updateAiOverlay = (data: {
  enabled: boolean;
  show_label?: boolean;
  show_confidence?: boolean;
  line_thickness?: number;
}) => request.put('/api/v1/media/ai-overlay', data, { silent: true });

// ==================== OSD Types & API ====================

export interface OsdTextOverlay {
  id: string;
  text?: string;
  x: number;
  y: number;
  font_size?: number;
  text_color?: number;
  // Horizontal alignment: 0=LEFT, 1=CENTER, 2=RIGHT
  h_align?: number;
  // Vertical alignment: 0=TOP, 1=CENTER, 2=BOTTOM
  v_align?: number;
  enabled: boolean;
}

export interface OsdDateTimeOverlay {
  id: string;
  x: number;
  y: number;
  format?: string;
  font_size?: number;
  text_color?: number;
  h_align?: number;
  v_align?: number;
  enabled: boolean;
}

export interface OsdImageOverlay {
  id: string;
  image_path?: string;
  image_size?: number;
  x: number;
  y: number;
  width?: number;
  height?: number;
  h_align?: number;
  v_align?: number;
  enabled: boolean;
}

export interface StreamOsdConfig {
  stream_name: string;
  text_overlays?: OsdTextOverlay[];
  datetime_overlays?: OsdDateTimeOverlay[];
  image_overlays?: OsdImageOverlay[];
}

export interface OsdConfigResponse {
  streams: StreamOsdConfig[];
}

export const fetchOsdConfig = async (): Promise<OsdConfigResponse> => {
  const response = await request.get('/api/v1/media/osd');
  return response.data;
};

export const updateOsdConfig = async (data: {
  streams: StreamOsdConfig[];
  // Editor edit-mode: suppress baked text overlays so the HTML proxy is the
  // single text layer while editing (the device re-bakes on editor exit).
  suppress_bake?: boolean;
}): Promise<void> => {
  await request.put('/api/v1/media/osd', data, { silent: true });
};

export const uploadOsdImage = async (
  file: File
): Promise<{ path: string; size: number }> => {
  const formData = new FormData();
  formData.append('file', file);
  const res = await request.post('/api/v1/media/osd/upload-image', formData);
  return res.data?.data ?? res.data;
};

// OSD overlay assets (the baked-OSD font + uploaded images) live behind token
// auth. The platform-api auth middleware accepts ?token=<raw> (the same path
// WebSocket streams use), so we append it for <img>/@font-face requests that
// can't set an Authorization header. The stored token carries a "Bearer "
// prefix; strip it — ValidateToken expects the raw token.
const osdAssetUrl = (path: string): string => {
  const raw = (getItem<string>('token') || '').replace(/^Bearer\s+/i, '');
  const query = raw ? `?token=${encodeURIComponent(raw)}` : '';
  return `${path}${query}`;
};

// URL of the exact TTF camera-daemon bakes text/datetime OSD with, so the HTML
// text proxy renders in the identical font (no proxy-vs-baked mismatch).
export const osdFontUrl = (): string => osdAssetUrl('/api/v1/media/osd/font');

// URL of an uploaded OSD overlay image for the HTML image proxy. image_path on
// the overlay is the absolute device path; only the basename is used and the
// route is locked to the OSD image directory server-side.
export const osdImageUrl = (imagePath?: string): string => {
  if (!imagePath) return '';
  const base = imagePath.split(/[\\/]/).pop() ?? '';
  if (!base) return '';
  return osdAssetUrl(`/api/v1/media/osd/image/${encodeURIComponent(base)}`);
};

// ==================== Encoder Reconfig ====================

export interface EncoderReconfigRequest {
  stream_name: string;
  width?: number;
  height?: number;
  codec?: 'h264' | 'h265';
  bitrate_bps?: number;
  fps?: number;
  gop?: number;
}

export interface EncoderReconfigResponse {
  message: string;
  interrupt_ms: number;
}

export const reconfigureEncoder = (
  data: EncoderReconfigRequest
): Promise<EncoderReconfigResponse> => request.put('/api/v1/media/encoder/reconfig', data, { silent: true });

// PTZ API (Mock compat)
export const getPtzState = (): Promise<PtzState> => Promise.resolve({ zoom: 2.4, focus: 50, mode: 'auto' });
export const updatePtzState = (_data: Partial<PtzState>): Promise<PtzState> => Promise.resolve({ ..._data } as PtzState);
export const ptzMove = (_data: PtzMoveRequest): Promise<void> => Promise.resolve();

// ==================== Privacy Mask ====================

export interface PrivacyMaskRegion {
  id: string;
  name: string;
  enabled: boolean;
  points_x: number[];
  points_y: number[];
}

export interface PrivacyMaskConfig {
  enabled: boolean;
  color: number;
  blur_radius: number;
  regions: PrivacyMaskRegion[];
  dpm_enabled: boolean;
  dpm_labels: string;
  dpm_mode: string;
  dpm_color: number;
}

export const fetchPrivacyMaskConfig = async (): Promise<PrivacyMaskConfig> => {
  const response = await request.get('/api/v1/media/privacy-mask');
  return response.data;
};

export const updatePrivacyMaskConfig = async (
  data: Partial<PrivacyMaskConfig>
): Promise<void> => {
  await request.put('/api/v1/media/privacy-mask', data, { silent: true });
};
