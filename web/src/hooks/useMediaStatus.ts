import { useQuery } from '@tanstack/react-query';
import { getMediaStatus } from '@/services/media';

/**
 * 获取码流实时状态
 * 通过 GET /api/v1/media/status
 *
 * Shared by the media page (`/media`) and the devices page (`/image`,
 * overlay tab) — both need the live stream list (codec / width / height) for
 * codec-fallback logic and overlay letterbox geometry. Lives here (not under
 * `pages/media/`) so the devices page does not depend on a sibling page folder.
 */
export interface MediaStreamStatus {
  stream_id: string;
  status: 'active' | 'stopped' | 'error' | 'unknown' | string;
  has_encoder: boolean;
  codec: string;
  width: number;
  height: number;
  fps: number;
  bitrate_bps: number;
  gop: number;
}

export const useMediaStatus = () => useQuery<{ streams: MediaStreamStatus[] }>({
    queryKey: ['mediaStatus'],
    queryFn: async () => {
      const response = await getMediaStatus();
      return (response?.data ?? response) as { streams: MediaStreamStatus[] };
    },
    staleTime: 30000,
  });
