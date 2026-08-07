import type { MockMethod } from 'vite-plugin-mock';
import type { AiModelTemplate, InstalledAiModel } from '../types/aiModels';

export const mockAiModelTemplates: AiModelTemplate[] = [
  {
    id: 'yolo-v8-coco',
    name: 'YOLOv8 Object Detection',
    description:
      'State-of-the-art object detection model pre-trained on Microsoft COCO dataset. High accuracy and real-time performance.',
    version: '8.0.2',
    sizeMB: 85,
    author: 'Ultralytics',
    category: 'Object Detection',
  },
  {
    id: 'yolo-v5-face',
    name: 'YOLOv5 Face Recognition',
    description:
      'Optimized variant of YOLOv5 specifically trained for multi-scale face detection in crowded scenes.',
    version: '5.2.1',
    sizeMB: 45,
    author: 'VisionAI',
    category: 'Face Detection',
  },
  {
    id: 'yolo-nas',
    name: 'YOLO-NAS Pose',
    description:
      'Neural Architecture Search based YOLO for human pose estimation. Fast and highly accurate skeletal mapping.',
    version: '1.1.0',
    sizeMB: 120,
    author: 'Deci AI',
    category: 'Pose Estimation',
  },
  {
    id: 'yolo-world',
    name: 'YOLO-World Open Vocabulary',
    description:
      'Open-vocabulary object detection model capable of zero-shot detection for custom defined objects via text prompts.',
    version: '2.0.0',
    sizeMB: 210,
    author: 'Tencent',
    category: 'Zero-Shot Detection',
  },
];

export const mockInstalledAiModels: InstalledAiModel[] = [
  {
    id: 'yolo-v8-coco',
    name: 'YOLOv8 Object Detection',
    image: 'ultralytics/yolov8:latest',
    status: 'running',
    cpuUsage: 45.2,
    memoryUsageMB: 1250,
    memoryTotalGB: 7.7,
  },
  {
    id: 'yolo-v5-face',
    name: 'YOLOv5 Face Recognition',
    image: 'visionai/yolov5-face:v5.2.1',
    status: 'stopped',
    cpuUsage: 0,
    memoryUsageMB: 0,
    memoryTotalGB: 7.7,
  },
];

export default [
  {
    url: '/api/ai-models/templates',
    method: 'get',
    response: () => ({
      code: 200,
      data: Object.values(mockAiModelTemplates),
      message: 'success',
    }),
  },
  {
    url: '/api/ai-models/installed',
    method: 'get',
    response: () => ({
      code: 200,
      data: Object.values(mockInstalledAiModels),
      message: 'success',
    }),
  },
  {
    url: '/api/ai-models/install',
    method: 'post',
    response: ({ body }: any) => {
      const { id } = body;
      const tpl = mockAiModelTemplates.find(t => t.id === id);
      if (tpl && !mockInstalledAiModels.find(m => m.id === id)) {
        mockInstalledAiModels.push({
          id: tpl.id,
          name: tpl.name,
          image: `registry.camthink.ai/${tpl.id}:v${tpl.version}`,
          status: 'stopped',
          cpuUsage: 0,
          memoryUsageMB: 0,
          memoryTotalGB: 7.7,
        });
      }
      return {
        code: 200,
        data: null,
        message: 'success',
      };
    },
  },
] as MockMethod[];
