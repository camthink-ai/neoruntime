import type { TFunction } from 'i18next';
import type { ModelFieldDef, ModelPackagePrefill } from '@/hooks/useModels';

/**
 * Pure helpers for the model import wizard — extracted verbatim from the
 * dialog so they are unit-testable, mirroring apps/lib/importFlow.ts. The
 * shell and section components keep every side effect; everything here must
 * stay free of React and i18n state (text is resolved by the caller via
 * modelFormIssueText).
 */

/** Pages of the configure screen — each nav entry renders one page. */
export type ModelImportSectionId = 'basic_info' | 'output' | 'advanced';

/** Schema fields that only steer the postprocess plugin — inert in raw mode
 *  (their values were baked into the HEF at compile time). Labels stay
 *  editable: they are consumer-side metadata, not plugin parameters. */
export const POSTPROCESS_ONLY_FIELDS = [
  'postprocess_profile',
  'threshold',
  'max_detections',
  'nms_threshold',
];

/** Schema keys rendered on the output page rather than the basic-info page
 *  (POSTPROCESS_ONLY_FIELDS plus labels, which is metadata but belongs with
 *  the postprocess story). */
const POSTPROCESS_PAGE_FIELDS = [...POSTPROCESS_ONLY_FIELDS, 'labels'];

/** Plugin schema keys a custom `{…}` variant must carry — mirrors the
 *  backend guard so a broken blob is caught before the request leaves. */
export const CUSTOM_VARIANT_KEYS = [
  'backend_function',
  'iou_threshold',
  'detection_threshold',
  'output_activation',
  'label_offset',
  'max_boxes',
  'labels',
];

/** Verified postprocess functions. Generic single-argument ones hardcode a
 *  0.4 threshold and COCO labels, so the UI steers users away from them. */
export const SUPPORTED_BACKEND_FUNCTIONS = [
  'hailo_yolov8n',
  'hailo_yolov8s',
  'hailo_yolov8m',
  // Parking-lot custom model — its backend function is the network name
  // itself (device-verified 2026-09-02).
  'yolov5m_vehicles',
];

/** Runtime-safe model_id charset — mirrors the backend RegisterModel gate
 *  (letters/digits first, then letters, digits, '.', '_' or '-', max 64).
 *  The id flows into gRPC registrations and materialized runtime paths. */
export const MODEL_ID_PATTERN = /^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$/;

export interface ModelImportFormState {
  modelId: string;
  modelType: string;
  /** 'platform' = plugin-decoded results; 'raw' = bare tensors. */
  outputMode: string;
  variant: string;
  config: Record<string, unknown>;
}

/** Server response of the parse endpoint — shared by the shell and the
 *  source-screen upload slot. */
export interface ModelParseResult {
  file_hash: string;
  file_path: string;
  file_size: number;
  filename: string;
  network_name: string;
  vstream_info: string;
  suggested_type: string;
  format: string;
  /** Server classification of the compiled output: 'nms' | 'feature_map'. */
  output_format?: string;
  /** Present only for AMPK .bin imports — pre-fills the form. */
  package?: ModelPackagePrefill;
  input_width?: number;
  input_height?: number;
}

export const initialModelImportForm: ModelImportFormState = {
  modelId: '',
  modelType: '',
  outputMode: 'platform',
  variant: '',
  config: {},
};

/** One validation finding. `field` doubles as the touched-map key used by
 *  the inline (onBlur) display; `reason` is an i18n key suffix under
 *  sys.ai_models.form. */
export interface ModelFormIssue {
  field: string;
  section: ModelImportSectionId;
  reason: string;
  params?: Record<string, string | number>;
}

export interface ValidateModelFormCtx {
  isUpdate: boolean;
  /** Feature-map detection HEF cannot enter the plugin pipeline. */
  platformModeDisabled: boolean;
  /** Normalized ids of registered models; undefined/null = list not loaded
   *  yet, so the duplicate check is skipped (backend still fast-fails). */
  existingModelIds?: Set<string> | null;
  /** Schema fields of the currently selected type. */
  fields: ModelFieldDef[];
}

export function formatFileSize(bytes: number): string {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export function sanitizeModelId(name: string): string {
  return name
    .toLowerCase()
    .replace(/[^a-z0-9_-]/g, '_')
    .replace(/_+/g, '_')
    .replace(/^_|_$/g, '');
}

/** Network names that carry no identity — developer-center HEFs commonly
 * ship with the network literally named "model", which would pre-fill a
 * meaningless id. Pre-fill falls through those to the file name. */
const GENERIC_MODEL_NAMES = new Set(['model', 'network', 'net']);

/** Pre-fill priority for the model id field: an AMPK package's explicit
 *  model_id always wins, then a distinctive network name, then the file
 *  name without extension. Generic network names are skipped. */
export function suggestModelId(
  pkgModelId: string | undefined,
  networkName: string | undefined,
  filename: string | undefined
): string {
  if (pkgModelId) return pkgModelId;
  const fileStem = filename?.replace(/\.[^.]+$/, '') ?? '';
  const network = networkName?.trim() ?? '';
  if (network && !GENERIC_MODEL_NAMES.has(network.toLowerCase())) {
    return network;
  }
  return fileStem;
}

/** Client mirror of the server-side classifier: an output vstream named
 *  *_nms_postprocess means the HEF ships the NMS layer; anything else is a
 *  bare feature map the postprocess plugin cannot decode. */
export function classifyOutputFormat(
  vstreamInfo: string
): 'nms' | 'feature_map' | '' {
  if (!vstreamInfo) return '';
  return vstreamInfo.includes('_nms_postprocess') ? 'nms' : 'feature_map';
}

export type VariantIssue =
  | { kind: 'invalid-json' }
  | { kind: 'missing-keys'; keys: string[] }
  | { kind: 'unsupported-function'; fn: string };

/** Validate a custom variant blob client-side. Plain (non-JSON) variants
 *  keep their existing passthrough handling and always return null. */
export function checkCustomVariant(variant: string): VariantIssue | null {
  const trimmed = variant.trim();
  if (!trimmed.startsWith('{')) return null;
  let parsed: Record<string, unknown>;
  try {
    parsed = JSON.parse(trimmed) as Record<string, unknown>;
  } catch {
    return { kind: 'invalid-json' };
  }
  const missing = CUSTOM_VARIANT_KEYS.filter(k => !(k in parsed));
  if (missing.length > 0) {
    return { kind: 'missing-keys', keys: missing };
  }
  const fn = parsed.backend_function;
  if (typeof fn !== 'string' || !SUPPORTED_BACKEND_FUNCTIONS.includes(fn)) {
    return {
      kind: 'unsupported-function',
      fn: typeof fn === 'string' ? fn : String(fn),
    };
  }
  return null;
}

/** Variant check → form issue (lives on the advanced page), or null. */
export function variantFormIssue(variant: string): ModelFormIssue | null {
  const issue = checkCustomVariant(variant);
  if (!issue) return null;
  switch (issue.kind) {
    case 'invalid-json':
      return {
        field: 'variant',
        section: 'advanced',
        reason: 'variant_invalid_json',
      };
    case 'missing-keys':
      return {
        field: 'variant',
        section: 'advanced',
        reason: 'variant_missing_keys',
        params: { keys: issue.keys.join(', ') },
      };
    case 'unsupported-function':
      return {
        field: 'variant',
        section: 'advanced',
        reason: 'variant_function_unsupported',
        params: { fns: SUPPORTED_BACKEND_FUNCTIONS.join(', ') },
      };
    default:
      return null;
  }
}

/** Resolve an issue to user-facing text — shared by the live inline errors
 *  and the submit-time toast. */
export function modelFormIssueText(
  issue: ModelFormIssue,
  t: TFunction
): string {
  const key = `sys.ai_models.form.${issue.reason}`;
  return issue.params ? t(key, issue.params) : t(key);
}

/** Postprocess profile basename → plugin backend function. */
export function backendFunctionForProfile(profile: string): string {
  if (profile.includes('yolov5m_vehicles')) return 'yolov5m_vehicles';
  if (profile.includes('yolov8s')) return 'hailo_yolov8s';
  if (profile.includes('yolov8m')) return 'hailo_yolov8m';
  return 'hailo_yolov8n';
}

/** Select options a schema field should render. Deployment-specific
 * ("custom") options — e.g. a customer-trained model verified against this
 * plugin build — surface only when they are already the active value
 * (auto-suggested from the parsed HEF, or loaded from the row being
 * updated); users without that deployment's model never see them. */
export function visibleSelectOptions<
  T extends { value: string; custom?: boolean },
>(options: T[], currentValue: unknown): T[] {
  const current = currentValue === undefined ? '' : String(currentValue);
  return options.filter(o => !o.custom || o.value === current);
}

/** Suggest a postprocess profile from the parsed HEF's vstream info: NMS
 * tensor names are <HEF basename>/…_nms_postprocess, so a schema option
 * whose value appears as a tensor-name prefix means this file IS that
 * profile's model. This is also what surfaces a custom option on a fresh
 * plain-HEF import — the dropdown hides custom entries that are not the
 * active value. Package metadata, when present, still wins. */
export function suggestPostprocessProfile(
  options: { value: string }[],
  vstreamInfo?: string
): string | null {
  if (!vstreamInfo) return null;
  for (const o of options) {
    if (vstreamInfo.includes(`${o.value}/`)) return o.value;
  }
  return null;
}

export function fieldDefaultToState(
  fields: ModelFieldDef[]
): Record<string, unknown> {
  const config: Record<string, unknown> = {};
  for (const f of fields) {
    if (f.default !== undefined) {
      config[f.key] = f.default;
    }
  }
  return config;
}

/** Config after switching model type: the new type's defaults, overlaid
 *  with previously-entered values for keys the new type ALSO understands —
 *  a detection→pose switch keeps a tuned threshold instead of wiping it. */
export function mergeConfigOnTypeSwitch(
  prevConfig: Record<string, unknown>,
  nextFields: ModelFieldDef[]
): Record<string, unknown> {
  const config = fieldDefaultToState(nextFields);
  for (const f of nextFields) {
    if (prevConfig[f.key] !== undefined) {
      config[f.key] = prevConfig[f.key];
    }
  }
  return config;
}

/** A model list row (or detail object) used to seed the update-mode form —
 *  the platform list persists config as raw JSON plus promoted top-level
 *  columns (threshold, max_detections, …). */
export interface UpdatePrefillSource {
  model_id: string;
  model_type?: string;
  output_mode?: string;
  variant?: string;
  config?: unknown;
  [key: string]: unknown;
}

/** Build the update-mode form from a registered model row: the selected
 *  type's schema defaults, overlaid with the row's persisted config (a raw
 *  object or a JSON string), overlaid with the promoted top-level columns.
 *  Columns win — they are the effective values the list displays. Without
 *  this merge, prefilling from the row's top-level keys alone loses every
 *  config-only value (labels, nms_threshold, postprocess_profile). */
export function prefillUpdateForm(
  model: UpdatePrefillSource,
  fields: ModelFieldDef[]
): ModelImportFormState {
  let stored: Record<string, unknown> = {};
  if (typeof model.config === 'string' && model.config.trim() !== '') {
    try {
      const parsed = JSON.parse(model.config) as unknown;
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        stored = parsed as Record<string, unknown>;
      }
    } catch {
      // Corrupt persisted config: fall through to defaults + columns.
    }
  } else if (
    model.config
    && typeof model.config === 'object'
    && !Array.isArray(model.config)
  ) {
    stored = model.config as Record<string, unknown>;
  }

  const config = fieldDefaultToState(fields);
  for (const f of fields) {
    if (stored[f.key] !== undefined) {
      config[f.key] = stored[f.key];
    }
    const col = model[f.key];
    if (col !== undefined && col !== null && col !== '') {
      config[f.key] = col;
    }
  }

  return {
    modelId: model.model_id,
    modelType: model.model_type ?? '',
    outputMode: model.output_mode === 'raw' ? 'raw' : 'platform',
    variant: model.variant ?? '',
    config,
  };
}

/** Which configure page a schema field renders on. */
export function sectionForField(key: string): ModelImportSectionId {
  return POSTPROCESS_PAGE_FIELDS.includes(key) ? 'output' : 'basic_info';
}

/** Split a type's schema fields between the two pages that render them
 *  (schema order preserved within each group). */
export function partitionFields(fields: ModelFieldDef[]): {
  basic: ModelFieldDef[];
  postprocess: ModelFieldDef[];
} {
  const basic: ModelFieldDef[] = [];
  const postprocess: ModelFieldDef[] = [];
  for (const f of fields) {
    if (POSTPROCESS_PAGE_FIELDS.includes(f.key)) postprocess.push(f);
    else basic.push(f);
  }
  return { basic, postprocess };
}

/** The registration payload the current form will submit — the data source
 *  of the read-only JSON preview (a projection, never parsed back). */
export function buildRegisterPreview(
  form: ModelImportFormState
): Record<string, unknown> {
  return {
    model_id: form.modelId.trim(),
    model_type: form.modelType,
    output_mode: form.outputMode,
    config: { ...form.config },
    model_variant: form.variant.trim(),
  };
}

/** Full form validation, shared by the inline (onBlur, per-field) display
 *  and the submit gate. Order is stable: id, type, variant, output mode,
 *  then dynamic fields in schema order — the shell toasts issues[0] and
 *  jumps to its section. */
export function validateModelForm(
  form: ModelImportFormState,
  ctx: ValidateModelFormCtx
): ModelFormIssue[] {
  const issues: ModelFormIssue[] = [];
  const modelIdLower = form.modelId.trim().toLowerCase();

  if (!form.modelId.trim()) {
    issues.push({
      field: 'modelId',
      section: 'basic_info',
      reason: 'required',
    });
  } else if (!MODEL_ID_PATTERN.test(form.modelId.trim())) {
    issues.push({
      field: 'modelId',
      section: 'basic_info',
      reason: 'model_id_invalid',
    });
  } else if (!ctx.isUpdate && ctx.existingModelIds?.has(modelIdLower)) {
    issues.push({
      field: 'modelId',
      section: 'basic_info',
      reason: 'model_id_exists',
    });
  }

  if (!form.modelType) {
    issues.push({
      field: 'modelType',
      section: 'basic_info',
      reason: 'required',
    });
  }

  const variantIssue = variantFormIssue(form.variant);
  if (variantIssue) issues.push(variantIssue);

  // The platform card disables itself for feature-map detection HEFs; if
  // the state still says platform (type just switched), block submit here
  // rather than letting the backend reject the request.
  if (ctx.platformModeDisabled && form.outputMode === 'platform') {
    issues.push({
      field: 'outputMode',
      section: 'output',
      reason: 'output_mode_invalid',
    });
  }

  for (const f of ctx.fields) {
    const field = `config_${f.key}`;
    const value = form.config[f.key];
    if (f.required && (value === undefined || value === '')) {
      issues.push({
        field,
        section: sectionForField(f.key),
        reason: 'required',
      });
      continue;
    }

    if (f.type === 'number' && value !== undefined && value !== '') {
      const n = typeof value === 'number' ? value : Number(value);
      if (!Number.isFinite(n)) {
        issues.push({
          field,
          section: sectionForField(f.key),
          reason: 'invalid_number',
        });
        continue;
      }

      // Special validation for common detection thresholds
      if (f.key === 'threshold' && (n < 0 || n > 1)) {
        issues.push({
          field,
          section: sectionForField(f.key),
          reason: 'threshold_range',
        });
        continue;
      }
      if (f.key === 'nms_threshold' && (n < 0 || n > 1)) {
        issues.push({
          field,
          section: sectionForField(f.key),
          reason: 'nms_threshold_range',
        });
        continue;
      }

      // Generic min/max validation if provided by capability schema
      if (typeof f.min === 'number' && n < f.min) {
        issues.push({
          field,
          section: sectionForField(f.key),
          reason: 'number_min',
          params: { min: f.min },
        });
        continue;
      }
      if (typeof f.max === 'number' && n > f.max) {
        issues.push({
          field,
          section: sectionForField(f.key),
          reason: 'number_max',
          params: { max: f.max },
        });
      }
    }
  }

  return issues;
}
