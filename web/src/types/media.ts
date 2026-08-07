// 编码器配置 — camera-daemon.yaml encoders[]
export interface EncoderConfig {
  stream_name: string; // "main" | "sub" | "third"
  codec: string; // "h264" | "h265"
  width: number;
  height: number;
  fps: number;
  bitrate: number;
  gop: number;
  enabled: boolean;
  osd_config_path?: string;
}

// AI 叠加层 — camera-daemon.yaml ai_overlay
export interface AIOverlayConfig {
  enabled: boolean;
  draw_labels: boolean;
  draw_confidence: boolean;
  draw_landmarks: boolean;
  enable_face_blur: boolean;
  box_thickness: number;
  stream_map?: string;
}

// ISP 图像调节 — 使用 settings.ts 中的完整 ISPConfig 定义
// re-export for convenience
export type { ISPConfig } from '@/services/settings';

// camera-daemon.yaml 完整配置（GET /api/v1/media/config 返回）
export interface CameraDaemonConfig {
  encoders: EncoderConfig[];
  ai_overlay: AIOverlayConfig;
  rtsp: { enabled: boolean; url?: string };
  streams?: Array<{
    name: string;
    width: number;
    height: number;
    fps: number;
    pool_max_buffers?: number;
  }>;
  [key: string]: any;
}

// PTZ 状态
export interface PtzState {
  zoom: number;
  focus: number;
  mode: 'auto' | 'manual';
}

// PTZ 移动请求
export interface PtzMoveRequest {
  direction:
    | 'up'
    | 'down'
    | 'left'
    | 'right'
    | 'up-left'
    | 'up-right'
    | 'down-left'
    | 'down-right'
    | 'home';
  speed?: number;
}
