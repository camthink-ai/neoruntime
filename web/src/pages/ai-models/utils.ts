/**
 * Canonical model type definitions.
 * Single source of truth for the frontend, aligned with
 * platform/platform-api/model/model_types.go.
 */

export type ModelTypeKey =
  | 'detection'
  | 'classification'
  | 'segmentation'
  | 'keypoint'
  | 'clip'
  | 'embedding'
  | 'ocr_detection'
  | 'ocr_recognition'
  | 'depth'
  | 'genai';

/** Resolve model_type to canonical key, with heuristic fallback from model_id. */
export function resolveModelType(
  modelType?: string,
  modelId?: string
): ModelTypeKey | null {
  if (modelType) {
    const key = modelType.toLowerCase() as ModelTypeKey;
    if (CANONICAL_TYPES.has(key)) return key;
  }

  const id = (modelId || '').toLowerCase();

  // Order matters — longer substrings first to avoid partial matches
  if (id.includes('ocr_rec')) return 'ocr_recognition';
  if (id.includes('ocr_det')) return 'ocr_detection';
  if (id.includes('embed')) return 'embedding';
  if (id.includes('depth') || id.includes('scdepth')) return 'depth';
  if (id.includes('qwen') || id.includes('genai') || id.includes('vlm')) return 'genai';
  if (id.includes('clip')) return 'clip';
  if (id.includes('seg')) return 'segmentation';
  if (id.includes('cls') || id.includes('class')) return 'classification';
  if (
    id.includes('face')
    || id.includes('landmark')
    || id.includes('pose')
    || id.includes('keypoint')
  ) return 'keypoint';
  if (id.includes('detect') || id.includes('yolo')) return 'detection';

  return null;
}

/** Get localized model type label. */
export function getModelTypeLabel(
  modelType: string | undefined,
  modelId: string | undefined,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  t: any
): string {
  const key = resolveModelType(modelType, modelId);
  if (key) return t(`sys.ai_models.model_type.${key}`);
  if (modelType) return modelType;
  return t('sys.ai_models.type.ai', 'AI Model');
}

/** Get localized model type description (for detail views). */
export function getModelTypeDescription(
  modelType: string | undefined,
  modelId: string | undefined,
  // eslint-disable-next-line @typescript-eslint/no-explicit-any
  t: any
): string {
  const key = resolveModelType(modelType, modelId);
  if (key) return t(`sys.ai_models.desc.${key}`);
  return t('sys.ai_models.desc.default', 'General AI Inference Model');
}

const CANONICAL_TYPES = new Set<ModelTypeKey>([
  'detection',
  'classification',
  'segmentation',
  'keypoint',
  'clip',
  'embedding',
  'ocr_detection',
  'ocr_recognition',
  'depth',
  'genai',
]);
