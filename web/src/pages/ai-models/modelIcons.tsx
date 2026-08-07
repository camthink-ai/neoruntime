import type { FC } from 'react';
import {
  Brain,
  Box,
  FileSearch,
  Layers,
  MessageSquareText,
  Mountain,
  Scan,
  Smile,
  Type,
  VectorSquare,
} from 'lucide-react';
import { resolveModelType, type ModelTypeKey } from './utils';

const MODEL_ICONS: Record<ModelTypeKey, FC<{ className?: string }>> = {
  detection: Scan,
  classification: Layers,
  segmentation: Box,
  keypoint: Smile,
  clip: Brain,
  embedding: VectorSquare,
  ocr_detection: FileSearch,
  ocr_recognition: Type,
  depth: Mountain,
  genai: MessageSquareText,
};

export function getModelIcon(
  modelType: string | undefined,
  modelId: string,
  className: string
) {
  const key = resolveModelType(modelType, modelId);
  if (key) {
    const Icon = MODEL_ICONS[key];
    if (Icon) return <Icon className={className} />;
  }
  return <Brain className={className} />;
}
