import { useQuery } from '@tanstack/react-query';
import { getMediaStatus } from '@/services/media';

export interface Stream {
  stream_id: string;
  status: string;
  has_encoder: boolean;
  codec: string;
  width: number;
  height: number;
  fps: number;
  bitrate_bps: number;
  gop?: number;
  h264_url?: string;
}

export const useStreams = (options?: { enabled?: boolean }) => useQuery<Stream[]>({
    queryKey: ['mediaStatus', 'streams'],
    queryFn: async () => {
      const response = await getMediaStatus();
      const data = response?.data ?? response;
      const streams: any[] = data?.streams ?? [];
      return streams.map(s => ({
        stream_id: s.stream_id,
        status: s.status,
        has_encoder: !!s.has_encoder,
        codec: s.codec ?? 'h264',
        width: s.width ?? 1920,
        height: s.height ?? 1080,
        fps: s.fps ?? 30,
        bitrate_bps: s.bitrate_bps ?? 4000000,
        h264_url: `/api/v1/h264/${s.stream_id}`,
      }));
    },
    staleTime: 60000,
    enabled: options?.enabled !== false,
  });

export const useStreamInfo = (streamId: string) => useQuery({
    queryKey: ['mediaStatus', 'streams', streamId],
    queryFn: async () => {
      const response = await getMediaStatus();
      const data = response?.data ?? response;
      const streams: any[] = data?.streams ?? [];
      return streams.find((s: any) => s.stream_id === streamId);
    },
    enabled: !!streamId,
  });
