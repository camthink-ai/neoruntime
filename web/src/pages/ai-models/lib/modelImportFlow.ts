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
  'keypoint_threshold',
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
  | { kind: 'unsupported-function'; fn: string }
  | { kind: 'bare-name' }
  | { kind: 'forbidden-keys'; keys: string[] };

/** Loader control keys the HAL postprocess layer dlopens from — refused on
 *  every variant surface regardless of model type (mirrors the registry's
 *  ForbiddenVariantKeys and the REST/runtime validators). */
export const FORBIDDEN_VARIANT_KEYS = [
  'backend_lib_path',
  'backend_config_path',
];

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

/** Validate a keypoint variant blob client-side, mirroring the REST
 * validateKeypointVariant dialect: '' is fine (the load-time profile
 * synthesis takes over), a {…} blob must be a flat JSON object whose keys
 * avoid the loader control keys — anything else passes, the channel is
 * deliberately open and content is HAL's to validate — and a bare name is
 * rejected (the runtime validator refuses it). */
export function checkKeypointVariant(variant: string): VariantIssue | null {
  const trimmed = variant.trim();
  if (trimmed === '') return null;
  if (!trimmed.startsWith('{')) return { kind: 'bare-name' };
  let parsed: unknown;
  try {
    parsed = JSON.parse(trimmed) as unknown;
  } catch {
    return { kind: 'invalid-json' };
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return { kind: 'invalid-json' };
  }
  const forbidden = FORBIDDEN_VARIANT_KEYS.filter(
    k => k in (parsed as Record<string, unknown>)
  );
  if (forbidden.length > 0) {
    return { kind: 'forbidden-keys', keys: forbidden };
  }
  return null;
}

/** Variant check → form issue (lives on the advanced page), or null. The
 * modelType dispatch mirrors the REST validator: detection = the closed
 * plugin schema, keypoint = the open dialect minus the loader control
 * keys, other types have no variant surface at all. */
export function variantFormIssue(
  variant: string,
  modelType: string
): ModelFormIssue | null {
  const issue =    modelType === 'detection'
      ? checkCustomVariant(variant)
      : modelType === 'keypoint'
        ? checkKeypointVariant(variant)
        : null;
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
    case 'bare-name':
      return {
        field: 'variant',
        section: 'advanced',
        reason: 'variant_bare_name',
      };
    case 'forbidden-keys':
      return {
        field: 'variant',
        section: 'advanced',
        reason: 'variant_forbidden_keys',
        params: { keys: issue.keys.join(', ') },
      };
    // The union is fully covered above; this only satisfies exhaustiveness
    // for a hypothetical future kind (reported as "no issue").
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

/** Inputs of buildVariantTemplate — the current form plus the parsed HEF's
 * network dimensions (the keypoint blob records them at create time). */
export interface VariantTemplateInput {
  modelType: string;
  config: Record<string, unknown>;
  inputWidth?: number;
  inputHeight?: number;
}

/** A schema-complete variant template from the current form values — the
 * starting point for the advanced escape hatch. Detection keeps the closed
 * seven-key plugin blob; keypoint mirrors the load-time composer
 * (KeypointVariantJSON) so the inserted text is exactly what leaving the
 * field empty would synthesize, ready to hand-tune. Other model types have
 * no variant surface — null means "nothing to insert". */
export function buildVariantTemplate(
  input: VariantTemplateInput
): string | null {
  if (input.modelType === 'detection') {
    const profile =      typeof input.config.postprocess_profile === 'string'
        ? input.config.postprocess_profile
        : 'hailo_yolov8n_384_640';
    // 0 is a legal threshold on both detection knobs (schema min 0 —
    // "detect everything"); only the box count needs > 0 (schema min 1).
    // Dropping a legal 0 here would silently re-pin the default 0.25 into
    // the template — the exact class of lie this primer exists to avoid.
    const num = (v: unknown, fallback: number, allowZero = false) => (typeof v === 'number' && Number.isFinite(v) && (allowZero || v > 0)
        ? v
        : fallback);
    const rawLabels =      typeof input.config.labels === 'string' ? input.config.labels : '';
    const labels = rawLabels
      .split(',')
      .map(s => s.trim())
      .filter(s => s !== '');
    return JSON.stringify(
      {
        backend_function: backendFunctionForProfile(profile),
        iou_threshold: num(input.config.nms_threshold, 0.45, true),
        detection_threshold: num(input.config.threshold, 0.25, true),
        output_activation: 'none',
        label_offset: 1,
        max_boxes: num(input.config.max_detections, 64),
        // Index 0 is a placeholder so labels[N] names class_id N.
        labels: ['unlabeled', ...labels],
      },
      null,
      2
    );
  }
  if (input.modelType === 'keypoint') {
    // The template is the pose composer's output; under the facial profile
    // there is nothing to compose — the decoder reads no configuration, and
    // inserting a pose blob here would silently WIN over the profile at
    // load time (escape-hatch precedence) and switch decoders outright.
    // An absent profile means facial: legacy keypoint rows predate the
    // profile key, and filterFieldsByProfile plus the loader's compatibility
    // behavior both read "missing" as facial_landmarks — composing a pose
    // blob for them would silently switch decoders on rows that never
    // opted in.
    const profile =      typeof input.config.postprocess_profile === 'string'
        ? input.config.postprocess_profile
        : 'facial_landmarks';
    if (profile === 'facial_landmarks') {
      return null;
    }
    // Same windows as modelload: score_threshold lives on [0.01, 1] and is
    // omitted outside it (HAL silently falls back to 0.6 below 1e-6);
    // keypoint_threshold on [0, 1]; network dims 16..4096, explicit config
    // key over the parsed dimension, absent when neither is in range.
    const blob: Record<string, unknown> = { native_yolov8_pose: true };
    const score =      typeof input.config.threshold === 'number'
      && Number.isFinite(input.config.threshold)
        ? input.config.threshold
        : 0.25;
    if (score >= 0.01 && score <= 1) {
      blob.score_threshold = score;
    }
    const kp =      typeof input.config.keypoint_threshold === 'number'
      && Number.isFinite(input.config.keypoint_threshold)
        ? input.config.keypoint_threshold
        : 0.25;
    if (kp >= 0 && kp <= 1) {
      blob.keypoint_threshold = kp;
    }
    const dim = (configKey: string, parsed: number | undefined) => {
      const explicit = input.config[configKey];
      if (
        typeof explicit === 'number'
        && Number.isFinite(explicit)
        && explicit >= 16
        && explicit <= 4096
      ) {
        return explicit;
      }
      if (typeof parsed === 'number' && parsed >= 16 && parsed <= 4096) {
        return parsed;
      }
      return undefined;
    };
    const width = dim('yolov8_pose_network_width', input.inputWidth);
    const height = dim('yolov8_pose_network_height', input.inputHeight);
    if (width !== undefined) blob.yolov8_pose_network_width = width;
    if (height !== undefined) blob.yolov8_pose_network_height = height;
    return JSON.stringify(blob, null, 2);
  }
  return null;
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

/** Suggest a keypoint postprocess profile from the parsed HEF's identity:
 * pose networks carry pose-shaped vstream/network names, face-landmark
 * networks face/landmark-shaped ones. null = no opinion — the caller keeps
 * the facial default (missing profile == facial == legacy behavior). */
export function suggestKeypointProfile(
  vstreamInfo: string | undefined | null,
  networkName?: string
): 'yolov8_pose' | 'facial_landmarks' | null {
  const hay = `${vstreamInfo ?? ''}\n${networkName ?? ''}`.toLowerCase();
  if (!hay.trim()) return null;
  if (hay.includes('pose')) return 'yolov8_pose';
  if (hay.includes('face') || hay.includes('landmark')) {
    return 'facial_landmarks';
  }
  return null;
}

/** Fields visible under the active postprocess profile: a field carrying a
 * non-empty profiles list renders only when that profile is active;
 * profile-less fields (the select itself, and every other type's controls)
 * are unconditionally visible. A missing profile value behaves as the
 * facial default — its pose-only controls stay hidden, matching legacy
 * rows that predate the profile key. */
export function filterFieldsByProfile(
  fields: ModelFieldDef[],
  profile: string | undefined | null
): ModelFieldDef[] {
  const active = profile ?? '';
  return fields.filter(
    f => !f.profiles || f.profiles.length === 0 || f.profiles.includes(active)
  );
}

/** A parsed vstream_info payload — the parse endpoint returns the HEF info
 * as a JSON string; the raw preview card renders its stream rows. */
export interface VStreamTable {
  networkName: string;
  /** Output-section stream rows only — never input streams or section
   * headers (review 2026-09-21). */
  vstreams: string[];
}

/** Output-section rows from parse-hef text: walks lines with the same
 * section tracking the server's parseHEFInfo uses, skipping the section
 * headers themselves (lines ending with ':'). Payloads without any
 * direction markers fall back to every non-empty line, so legacy arrays
 * predating the markers still show their rows. */
export function outputVStreamRows(lines: string[]): string[] {
  const rows: string[] = [];
  let section = '';
  for (const raw of lines) {
    const line = raw.trim();
    if (line === '') continue;
    const lower = line.toLowerCase();
    if (lower.includes('input') && lower.includes('vstream')) {
      section = 'input';
    } else if (lower.includes('output') && lower.includes('vstream')) {
      section = 'output';
    } else if (lower.startsWith('input')) {
      section = 'input';
    } else if (lower.startsWith('output')) {
      section = 'output';
    }
    const isStreamLine =      lower.includes('vstream')
      || lower.includes('stream')
      || lower.startsWith('input')
      || lower.startsWith('output');
    if (section === 'output' && isStreamLine && !line.endsWith(':')) {
      rows.push(line);
    }
  }
  if (section === '') {
    return lines.map(l => l.trim()).filter(l => l !== '');
  }
  return rows;
}

/** Tolerant parse of the parse endpoint's vstream_info: a JSON string
 * carrying {network_name, output_vstreams[], vstreams[], raw_output}.
 * Prefers the server-parsed output_vstreams array; legacy vstreams arrays
 * and raw_output text are direction-filtered client-side. Anything
 * malformed, empty, or shapeless yields null so the preview card hides. */
export function parseVStreamTable(
  vstreamInfo: string | undefined | null
): VStreamTable | null {
  if (!vstreamInfo) return null;
  let parsed: unknown;
  try {
    parsed = JSON.parse(vstreamInfo) as unknown;
  } catch {
    return null;
  }
  if (!parsed || typeof parsed !== 'object' || Array.isArray(parsed)) {
    return null;
  }
  const obj = parsed as Record<string, unknown>;
  const networkName =    typeof obj.network_name === 'string' ? obj.network_name : '';
  const stringRows = (v: unknown): string[] => (Array.isArray(v)
      ? v.filter((s): s is string => typeof s === 'string' && s.trim() !== '')
      : []);

  const direct = stringRows(obj.output_vstreams);
  if (direct.length > 0) return { networkName, vstreams: direct };
  if (Array.isArray(obj.vstreams)) {
    const rows = outputVStreamRows(stringRows(obj.vstreams));
    if (rows.length > 0) return { networkName, vstreams: rows };
    return null;
  }
  if (typeof obj.raw_output === 'string' && obj.raw_output.trim() !== '') {
    const rows = outputVStreamRows(obj.raw_output.split('\n'));
    if (rows.length > 0) return { networkName, vstreams: rows };
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
 *  a detection→pose switch keeps a tuned threshold instead of wiping it.
 *  Select values additionally survive only when the new type offers them:
 *  profile keys are type-local (a detection basename is not a keypoint
 *  decoder id and vice versa), and carrying a now-foreign value would
 *  prefill the select with something its own dropdown cannot display. */
export function mergeConfigOnTypeSwitch(
  prevConfig: Record<string, unknown>,
  nextFields: ModelFieldDef[]
): Record<string, unknown> {
  const config = fieldDefaultToState(nextFields);
  for (const f of nextFields) {
    if (prevConfig[f.key] === undefined) continue;
    if (f.options && f.options.length > 0) {
      const legal = f.options.some(o => o.value === String(prevConfig[f.key]));
      if (!legal) continue; // fall back to the new type's default
    }
    config[f.key] = prevConfig[f.key];
  }
  return config;
}

/** Whether a variant string overrides the postprocess profile at load time.
 *  A `{`-leading blob wins over the profile selection (device-verified
 *  escape-hatch precedence), so any flow that changes the profile must
 *  clear such a blob first or the change silently does nothing. Pure so
 *  the behavior is unit-testable without a component tree. */
export function variantOverridesProfile(variant: string): boolean {
  return variant.trimStart().startsWith('{');
}

/** Field-dialect → variant-blob dialect bridge for PRESENTATION ONLY (the
 *  detail page's effective-config panel and the import wizard's templates).
 *  Mirrors the Go registry's paramKeyToFieldKey; the two namespaces spell
 *  the same knob differently and validation never walks this table. */
export const fieldKeyToVariantKey: Record<string, string> = {
  threshold: 'detection_threshold', // detection dialect
  max_detections: 'max_boxes',
  nms_threshold: 'iou_threshold',
  labels: 'labels',
};

/** Keypoint's blob spells the score knob differently from detection's. */
export const fieldKeyToPoseVariantKey: Record<string, string> = {
  threshold: 'score_threshold',
  keypoint_threshold: 'keypoint_threshold',
};

/** What the runtime postprocess actually receives for a model — the detail
 *  page's "effective config" projection. Resolution order mirrors the
 *  load-time composer exactly, so the panel cannot claim something the
 *  pipeline would not honor:
 *   - raw mode sends neither type nor variant → no postprocess payload;
 *   - a `{`-blob variant wins verbatim (escape-hatch precedence);
 *   - otherwise the composer synthesizes from config + profile (detection
 *     seven-key schema / keypoint pose blob; facial composes nothing);
 *   - a bare-name variant passes through as the plugin routing name.
 */
export interface EffectivePostprocess {
  source:
    | 'custom-blob' // '{'-variant wins verbatim over the composed config
    | 'composed' // synthesized from config + profile at load time
    | 'passthrough-name' // bare plugin routing name (non-detection types)
    | 'none-raw' // raw delivery: no postprocess payload at all
    | 'none'; // no variant surface for this type/profile
  /** Parsed blob for 'custom-blob'/'composed'; null otherwise (and for a
   *  custom blob that fails to parse — stale data the panel flags). */
  blob: Record<string, unknown> | null;
  /** The verbatim variant string behind a 'custom-blob' source. */
  rawVariant?: string;
}

export function effectivePostprocess(input: {
  modelType: string;
  outputMode: string;
  variant: string;
  config: Record<string, unknown>;
  inputWidth?: number;
  inputHeight?: number;
}): EffectivePostprocess {
  if (input.outputMode === 'raw') {
    return { source: 'none-raw', blob: null };
  }
  const variant = input.variant.trim();
  if (variantOverridesProfile(variant)) {
    try {
      const parsed: unknown = JSON.parse(variant);
      if (parsed && typeof parsed === 'object' && !Array.isArray(parsed)) {
        return {
          source: 'custom-blob',
          blob: parsed as Record<string, unknown>,
          rawVariant: variant,
        };
      }
    } catch {
      // Corrupt stored blob — the panel flags it instead of guessing.
    }
    return { source: 'custom-blob', blob: null, rawVariant: variant };
  }
  if (variant !== '' && input.modelType !== 'detection') {
    // Non-detection bare names reach the runtime verbatim — the loader's
    // registration path passes non-detection variants through unchanged.
    return { source: 'passthrough-name', blob: null, rawVariant: variant };
  }
  // A bare DETECTION name is not a passthrough either: the loader's
  // DetectionVariantJSON replaces every non-JSON detection variant with the
  // full blob composed from the selected profile and stored thresholds (and
  // the runtime expands a directly supplied bare name the same way), so the
  // panel shows what will actually run instead of claiming verbatim routing.
  const template = buildVariantTemplate({
    modelType: input.modelType,
    config: input.config,
    inputWidth: input.inputWidth,
    inputHeight: input.inputHeight,
  });
  if (template === null) {
    return variant !== ''
      ? { source: 'passthrough-name', blob: null, rawVariant: variant }
      : { source: 'none', blob: null };
  }
  return {
    source: 'composed',
    blob: JSON.parse(template) as Record<string, unknown>,
    ...(variant !== '' ? { rawVariant: variant } : {}),
  };
}

/** Whether submitting the current form would reload a loaded model. The
 *  REST update path reloads on file/type/output-mode/variant changes, and
 *  config keys reach the runtime only through the composed variant — so a
 *  composed-template difference (threshold, profile, labels, dims…) is a
 *  reload too. Pure: compare the prefilled snapshot against the live form.
 *  A file replacement is out of scope here — it is a different screen and
 *  reloads by definition. */
export function reloadRelevantChange(
  initial: ModelImportFormState | null | undefined,
  current: ModelImportFormState
): boolean {
  if (!initial) return false;
  if (
    initial.modelType !== current.modelType
    || initial.outputMode !== current.outputMode
    || initial.variant.trim() !== current.variant.trim()
  ) {
    return true;
  }
  const before = buildVariantTemplate({
    modelType: initial.modelType,
    config: initial.config,
  });
  const after = buildVariantTemplate({
    modelType: current.modelType,
    config: current.config,
  });
  return before !== after;
}

/** One changed key of an edit-mode submit diff: top-level payload keys plus
 *  per-key config rows (flattened as "config.<key>"), so a threshold tweak
 *  reads as one row instead of one opaque config-object row. */
export interface PreviewDiffRow {
  key: string;
  before: unknown;
  after: unknown;
}

/** Diff two register payloads (buildRegisterPreview output) down to changed
 *  keys — the edit screen's "what am I about to change" view. Added keys
 *  carry undefined before, removed keys undefined after. */
export function diffRegisterPreview(
  before: Record<string, unknown>,
  after: Record<string, unknown>
): PreviewDiffRow[] {
  const rows: PreviewDiffRow[] = [];
  const seen = new Set<string>();
  const emit = (key: string, b: unknown, a: unknown) => {
    if (JSON.stringify(b) !== JSON.stringify(a)) {
      rows.push({ key, before: b, after: a });
    }
  };
  for (const key of Object.keys(after)) {
    seen.add(key);
    if (key === 'config') continue;
    emit(key, before[key], after[key]);
  }
  for (const key of Object.keys(before)) {
    if (!seen.has(key) && key !== 'config') {
      emit(key, before[key], after[key]); // removed key (after undefined)
    }
  }
  const beforeConfig =    before.config && typeof before.config === 'object'
      ? (before.config as Record<string, unknown>)
      : {};
  const afterConfig =    after.config && typeof after.config === 'object'
      ? (after.config as Record<string, unknown>)
      : {};
  const configKeys = new Set([
    ...Object.keys(beforeConfig),
    ...Object.keys(afterConfig),
  ]);
  for (const key of configKeys) {
    emit(`config.${key}`, beforeConfig[key], afterConfig[key]);
  }
  return rows;
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

/** File-identity facts a replacement HEF adds to an update request — the
 *  same fields the import dialog threads from its parse result into
 *  UpdateModel. A replacement reloads the model even when every form field
 *  is unchanged, so the edit diff must surface them; the persisted payload
 *  carries none of these keys, so they render as added rows. */
export type ReplacementFileFacts = {
  file_hash?: string;
  file_size?: number;
  network_name?: string;
  vstream_info?: string;
  input_width?: number;
  input_height?: number;
};

/** Fold the replacement file's identity facts into a register preview.
 *  Only keys with a defined value are added (an unknown dimension must not
 *  appear as a change — UpdateModel treats an explicit 0 as "clear"). */
export function withReplacementFileFacts(
  preview: Record<string, unknown>,
  facts: ReplacementFileFacts | null | undefined
): Record<string, unknown> {
  if (!facts) return preview;
  const merged: Record<string, unknown> = { ...preview };
  const keys: (keyof ReplacementFileFacts)[] = [
    'file_hash',
    'file_size',
    'network_name',
    'vstream_info',
    'input_width',
    'input_height',
  ];
  for (const key of keys) {
    const value = facts[key];
    if (value !== undefined && value !== null) merged[key] = value;
  }
  return merged;
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

  const variantIssue = variantFormIssue(form.variant, form.modelType);
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
