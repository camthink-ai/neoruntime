import { describe, expect, it } from 'vitest';
import type { ModelFieldDef } from '@/hooks/useModels';
import {
  backendFunctionForProfile,
  buildRegisterPreview,
  buildVariantTemplate,
  checkCustomVariant,
  checkKeypointVariant,
  classifyOutputFormat,
  diffRegisterPreview,
  effectivePostprocess,
  fieldDefaultToState,
  fieldKeyToPoseVariantKey,
  fieldKeyToVariantKey,
  filterFieldsByProfile,
  formatFileSize,
  mergeConfigOnTypeSwitch,
  modelFormIssueText,
  MODEL_ID_PATTERN,
  outputVStreamRows,
  parseVStreamTable,
  partitionFields,
  prefillUpdateForm,
  reloadRelevantChange,
  sanitizeModelId,
  suggestKeypointProfile,
  suggestModelId,
  suggestPostprocessProfile,
  validateModelForm,
  variantFormIssue,
  withReplacementFileFacts,
  variantOverridesProfile,
  visibleSelectOptions,
  type ModelImportFormState,
  type ValidateModelFormCtx,
} from './modelImportFlow';

const validVariant = JSON.stringify({
  backend_function: 'hailo_yolov8s',
  iou_threshold: 0.45,
  detection_threshold: 0.25,
  output_activation: 'none',
  label_offset: 1,
  max_boxes: 64,
  labels: ['person'],
});

const detectionFields: ModelFieldDef[] = [
  { key: 'name', type: 'text', required: true },
  {
    key: 'postprocess_profile',
    type: 'select',
    required: true,
    default: 'yolov8n',
  },
  { key: 'threshold', type: 'number', required: true, default: 0.5 },
  { key: 'nms_threshold', type: 'number', default: 0.45 },
  { key: 'max_detections', type: 'number', default: 64, min: 1, max: 512 },
  { key: 'labels', type: 'text', default: '' },
];

const baseForm: ModelImportFormState = {
  modelId: 'yolov8n-demo',
  modelType: 'detection',
  outputMode: 'platform',
  variant: '',
  config: {
    name: 'demo',
    postprocess_profile: 'yolov8n',
    threshold: 0.5,
    nms_threshold: 0.45,
    max_detections: 64,
    labels: 'person',
  },
};

const baseCtx: ValidateModelFormCtx = {
  isUpdate: false,
  platformModeDisabled: false,
  existingModelIds: new Set(['taken-id']),
  fields: detectionFields,
};

describe('sanitizeModelId', () => {
  it.each([
    ['YoloV8n Demo', 'yolov8n_demo'],
    ['Fire & Smoke!!', 'fire_smoke'],
    ['__leading__', 'leading'],
    ['trailing___', 'trailing'],
    ['中文名称', ''], // non-ascii → underscores → collapsed → trimmed away
    ['already-clean_1', 'already-clean_1'],
    ['', ''],
  ])('sanitizeModelId(%j) → %j', (input, expected) => {
    expect(sanitizeModelId(input)).toBe(expected);
  });
});

describe('suggestModelId', () => {
  it('prefers the AMPK package model_id over everything', () => {
    expect(suggestModelId('pkg-id', 'yolov8n', 'file.hef')).toBe('pkg-id');
  });

  it('uses a distinctive network name when no package id exists', () => {
    expect(suggestModelId(undefined, 'fire_smoke_net', 'f.hef')).toBe(
      'fire_smoke_net'
    );
  });

  it.each(['model', 'network', 'net', 'Model', 'NETWORK'])(
    'falls back to the file stem for generic network name %j',
    generic => {
      expect(suggestModelId(undefined, generic, 'my_detector_v2.hef')).toBe(
        'my_detector_v2'
      );
    }
  );

  it('trims whitespace before judging the network name', () => {
    expect(suggestModelId(undefined, '  net  ', 'x.hef')).toBe('x');
    expect(suggestModelId(undefined, '  yolov8n  ', 'x.hef')).toBe('yolov8n');
  });

  it('returns the file stem when nothing else is known', () => {
    expect(suggestModelId(undefined, undefined, 'detector.hef')).toBe(
      'detector'
    );
  });

  it('yields empty for a truly empty input triple', () => {
    expect(suggestModelId(undefined, undefined, undefined)).toBe('');
    expect(suggestModelId(undefined, '', '')).toBe('');
  });

  it('strips only the last extension from the file stem', () => {
    expect(suggestModelId(undefined, 'net', 'archive.tar.hef')).toBe(
      'archive.tar'
    );
  });
});

describe('MODEL_ID_PATTERN', () => {
  it.each([
    ['yolov8n-demo', true],
    ['A', true],
    ['a'.repeat(64), true],
    ['Model_2.v-1', true],
    ['-leading-dash', false], // must start with letter/digit
    ['.dotted-start', false],
    ['my model', false], // space
    ['检测模型', false], // non-ascii
    ['a/b', false], // path separator
    ['a'.repeat(65), false], // over 64 chars
    ['', false],
  ])('MODEL_ID_PATTERN.test(%j) → %s', (input, expected) => {
    expect(MODEL_ID_PATTERN.test(input)).toBe(expected);
  });
});

describe('mergeConfigOnTypeSwitch', () => {
  const poseFields: ModelFieldDef[] = [
    { key: 'name', type: 'text', required: true },
    {
      key: 'postprocess_profile',
      type: 'select',
      default: 'yolov8_pose',
      options: [
        { value: 'facial_landmarks', label: 'facial' },
        { value: 'yolov8_pose', label: 'pose' },
      ],
    },
    { key: 'threshold', type: 'number', default: 0.5 },
    { key: 'max_detections', type: 'number', default: 64 },
    { key: 'keypoint_threshold', type: 'number', default: 0.3 },
  ];

  it('keeps previously-entered values for keys the new type also has', () => {
    const merged = mergeConfigOnTypeSwitch(
      { ...baseForm.config, threshold: 0.35, labels: 'person' },
      poseFields
    );
    // tuned threshold survives the detection → pose switch
    expect(merged.threshold).toBe(0.35);
    expect(merged.name).toBe('demo');
    // detection-only key is dropped, pose-only key falls back to its default
    expect(merged).not.toHaveProperty('labels');
    expect(merged).not.toHaveProperty('nms_threshold');
    expect(merged.keypoint_threshold).toBe(0.3);
  });

  it('falls back to the new type defaults when nothing was entered', () => {
    const merged = mergeConfigOnTypeSwitch({}, poseFields);
    expect(merged).toEqual(fieldDefaultToState(poseFields));
  });

  it('does not leak keys absent from the new schema even without defaults', () => {
    const merged = mergeConfigOnTypeSwitch({ stale_key: 1 }, poseFields);
    expect(merged).not.toHaveProperty('stale_key');
  });

  it('drops select values the new type does not offer', () => {
    // Profile keys are type-local: a detection basename carried into the
    // keypoint select would prefill a value its own dropdown cannot show.
    const merged = mergeConfigOnTypeSwitch(
      { postprocess_profile: 'hailo_yolov8n_384_640' },
      poseFields
    );
    expect(merged.postprocess_profile).toBe('yolov8_pose'); // new default
  });

  it('keeps select values the new type also offers', () => {
    const merged = mergeConfigOnTypeSwitch(
      { postprocess_profile: 'facial_landmarks' },
      poseFields
    );
    expect(merged.postprocess_profile).toBe('facial_landmarks');
  });
});

describe('classifyOutputFormat', () => {
  it.each([
    ['', ''],
    ['yolov8n/1, output1_nms_postprocess', 'nms'],
    ['yolov8n/1, output1', 'feature_map'],
  ])('classifyOutputFormat(%j) → %j', (input, expected) => {
    expect(classifyOutputFormat(input)).toBe(expected);
  });
});

describe('formatFileSize', () => {
  it.each([
    [512, '512 B'],
    [2048, '2.0 KB'],
    [5 * 1024 * 1024, '5.0 MB'],
  ])('formatFileSize(%d) → %j', (input, expected) => {
    expect(formatFileSize(input)).toBe(expected);
  });
});

describe('checkCustomVariant', () => {
  it('passes plain identifiers through untouched', () => {
    expect(checkCustomVariant('hailo_yolov8n')).toBeNull();
  });
  it('passes empty and whitespace-only values through', () => {
    expect(checkCustomVariant('')).toBeNull();
    expect(checkCustomVariant('   ')).toBeNull();
  });
  it('accepts a complete supported variant', () => {
    expect(checkCustomVariant(validVariant)).toBeNull();
  });
  it('rejects malformed JSON', () => {
    expect(checkCustomVariant('{not json')).toEqual({ kind: 'invalid-json' });
  });
  it('reports every missing required key', () => {
    const issue = checkCustomVariant('{"backend_function":"hailo_yolov8n"}');
    expect(issue).toMatchObject({ kind: 'missing-keys' });
    const { keys } = issue as { kind: 'missing-keys'; keys: string[] };
    expect(keys).toEqual(expect.arrayContaining(['iou_threshold', 'labels']));
    expect(keys).toHaveLength(6);
  });
  it('rejects an unsupported backend function', () => {
    const variant = JSON.stringify({
      ...JSON.parse(validVariant),
      backend_function: 'generic_v1',
    });
    expect(checkCustomVariant(variant)).toEqual({
      kind: 'unsupported-function',
      fn: 'generic_v1',
    });
  });
  it('rejects a non-string backend function', () => {
    const variant = JSON.stringify({
      ...JSON.parse(validVariant),
      backend_function: 42,
    });
    expect(checkCustomVariant(variant)).toMatchObject({
      kind: 'unsupported-function',
    });
  });
});

describe('variantFormIssue', () => {
  it('maps each variant issue onto the advanced section', () => {
    expect(variantFormIssue('', 'detection')).toBeNull();
    expect(variantFormIssue('{bad', 'detection')).toMatchObject({
      field: 'variant',
      section: 'advanced',
      reason: 'variant_invalid_json',
    });
    const missing = variantFormIssue(
      '{"backend_function":"hailo_yolov8n"}',
      'detection'
    );
    expect(missing).toMatchObject({
      field: 'variant',
      section: 'advanced',
      reason: 'variant_missing_keys',
    });
    expect(missing?.params?.keys).toContain('iou_threshold');
  });

  it('dispatches by model type like the REST validator (review 2026-09-21)', () => {
    // Detection's closed schema rejects the official pose blob…
    expect(
      variantFormIssue('{"native_yolov8_pose":true}', 'detection')
    ).toMatchObject({ reason: 'variant_missing_keys' });
    // …while keypoint accepts it (open dialect)…
    expect(
      variantFormIssue('{"native_yolov8_pose":true}', 'keypoint')
    ).toBeNull();
    // …rejects loader control keys…
    expect(
      variantFormIssue('{"backend_lib_path":"/x.so"}', 'keypoint')
    ).toMatchObject({
      field: 'variant',
      reason: 'variant_forbidden_keys',
      params: { keys: 'backend_lib_path' },
    });
    // …and rejects bare names.
    expect(variantFormIssue('yolov8s_pose', 'keypoint')).toMatchObject({
      field: 'variant',
      reason: 'variant_bare_name',
    });
    // Other model types have no variant surface: anything passes.
    expect(variantFormIssue('whatever', 'classification')).toBeNull();
    expect(variantFormIssue('{bad', 'classification')).toBeNull();
  });
});

describe('checkKeypointVariant', () => {
  it('passes empty values and flat JSON objects through', () => {
    expect(checkKeypointVariant('')).toBeNull();
    expect(checkKeypointVariant('   ')).toBeNull();
    // The official pose blob passes verbatim (open dialect, review F1).
    expect(
      checkKeypointVariant('{"native_yolov8_pose":true,"score_threshold":0.5}')
    ).toBeNull();
    // Unknown keys pass — the channel is opaque, content is HAL's to check.
    expect(checkKeypointVariant('{"anything":1}')).toBeNull();
  });
  it('rejects malformed or non-object JSON', () => {
    expect(checkKeypointVariant('{not json')).toEqual({ kind: 'invalid-json' });
    // A JSON array/scalar does not start with '{', so it lands on the
    // bare-name branch — exactly what the REST validator does with it.
    expect(checkKeypointVariant('["array"]')).toEqual({ kind: 'bare-name' });
  });
  it('rejects bare names and loader control keys', () => {
    expect(checkKeypointVariant('yolov8s_pose')).toEqual({ kind: 'bare-name' });
    const issue = checkKeypointVariant(
      '{"backend_lib_path":"/x.so","backend_config_path":"/y.json"}'
    );
    expect(issue).toEqual({
      kind: 'forbidden-keys',
      keys: ['backend_lib_path', 'backend_config_path'],
    });
  });
});

describe('buildVariantTemplate', () => {
  it('composes the closed seven-key detection template from the form', () => {
    const template = buildVariantTemplate({
      modelType: 'detection',
      config: {
        postprocess_profile: 'hailo_yolov8s_384_640',
        nms_threshold: 0.3,
        threshold: 0.4,
        max_detections: 32,
        labels: 'person, face',
      },
    });
    expect(JSON.parse(template ?? '')).toEqual({
      backend_function: 'hailo_yolov8s',
      iou_threshold: 0.3,
      detection_threshold: 0.4,
      output_activation: 'none',
      label_offset: 1,
      max_boxes: 32,
      labels: ['unlabeled', 'person', 'face'],
    });
  });

  it('composes the keypoint pose blob exactly like the load-time composer', () => {
    const template = buildVariantTemplate({
      modelType: 'keypoint',
      config: {
        postprocess_profile: 'yolov8_pose',
        threshold: 0.6,
        keypoint_threshold: 0.35,
      },
      inputWidth: 640,
      inputHeight: 640,
    });
    expect(JSON.parse(template ?? '')).toEqual({
      native_yolov8_pose: true,
      score_threshold: 0.6,
      keypoint_threshold: 0.35,
      yolov8_pose_network_width: 640,
      yolov8_pose_network_height: 640,
    });
  });

  it('omits out-of-window keypoint values and absent network dims', () => {
    // score_threshold floor is 0.01 (HAL falls back to 0.6 below 1e-6);
    // keypoint_threshold window is [0, 1]; no parsed dims → no dim keys.
    const template = buildVariantTemplate({
      modelType: 'keypoint',
      config: {
        postprocess_profile: 'yolov8_pose',
        threshold: 0,
        keypoint_threshold: 2,
      },
    });
    expect(JSON.parse(template ?? '')).toEqual({
      native_yolov8_pose: true,
    });
    // Absent config thresholds fall back to the composer's 0.25 defaults.
    const defaults = buildVariantTemplate({
      modelType: 'keypoint',
      config: { postprocess_profile: 'yolov8_pose' },
    });
    expect(JSON.parse(defaults ?? '')).toEqual({
      native_yolov8_pose: true,
      score_threshold: 0.25,
      keypoint_threshold: 0.25,
    });
  });

  it('returns null for model types without a variant surface', () => {
    expect(
      buildVariantTemplate({ modelType: 'classification', config: {} })
    ).toBeNull();
  });

  it('preserves a legal 0 detection threshold instead of re-pinning the default', () => {
    // Schema min is 0 — "detect everything" is a legitimate setting; the
    // template must carry it, not silently substitute 0.25.
    const template = buildVariantTemplate({
      modelType: 'detection',
      config: { threshold: 0, nms_threshold: 0 },
    });
    const blob = JSON.parse(template ?? '');
    expect(blob.detection_threshold).toBe(0);
    expect(blob.iou_threshold).toBe(0);
    // max_detections 0 is NOT legal (schema min 1) — the default applies.
    const zeroBoxes = buildVariantTemplate({
      modelType: 'detection',
      config: { max_detections: 0 },
    });
    expect(JSON.parse(zeroBoxes ?? '').max_boxes).toBe(64);
  });

  it('returns null for the keypoint facial profile (nothing composes)', () => {
    // A pose blob inserted under facial would WIN over the profile at load
    // time and switch decoders outright — there must be no template to insert.
    expect(
      buildVariantTemplate({
        modelType: 'keypoint',
        config: { postprocess_profile: 'facial_landmarks', threshold: 0.5 },
      })
    ).toBeNull();
  });

  it('treats an absent keypoint profile as facial (legacy rows)', () => {
    // Legacy keypoint rows predate postprocess_profile; the field filter and
    // the loader's compatibility behavior both read "missing" as facial, so
    // the template must not compose a pose blob for them either.
    expect(
      buildVariantTemplate({
        modelType: 'keypoint',
        config: { threshold: 0.6 },
        inputWidth: 640,
        inputHeight: 640,
      })
    ).toBeNull();
  });
});

describe('variantOverridesProfile', () => {
  it.each([
    ['{"native_yolov8_pose":true}', true],
    ['  \n{"backend_function":"hailo_yolov8n"}', true], // leading whitespace
    ['hailo_yolov8n', false],
    ['', false],
    ['   ', false],
  ])('variantOverridesProfile(%j) → %s', (input, expected) => {
    expect(variantOverridesProfile(input)).toBe(expected);
  });
});

describe('modelFormIssueText', () => {
  it('resolves the i18n key with params', () => {
    function t(key: string, params?: Record<string, unknown>) {
      return params ? `${key}:${JSON.stringify(params)}` : key;
    }
    expect(
      modelFormIssueText(
        {
          field: 'config_threshold',
          section: 'output',
          reason: 'threshold_range',
        },
        t as never
      )
    ).toBe('sys.ai_models.form.threshold_range');
    expect(
      modelFormIssueText(
        {
          field: 'config_max_detections',
          section: 'output',
          reason: 'number_min',
          params: { min: 1 },
        },
        t as never
      )
    ).toBe('sys.ai_models.form.number_min:{"min":1}');
  });
});

describe('backendFunctionForProfile', () => {
  it.each([
    ['yolov8n', 'hailo_yolov8n'],
    ['yolov8n_hef', 'hailo_yolov8n'],
    ['yolov8s', 'hailo_yolov8s'],
    ['yolov8m_2048', 'hailo_yolov8m'],
    ['yolov5m_vehicles', 'yolov5m_vehicles'],
  ])('profile %j → %j', (profile, expected) => {
    expect(backendFunctionForProfile(profile)).toBe(expected);
  });
});

describe('fieldDefaultToState', () => {
  it('keeps only fields that declare a default', () => {
    expect(fieldDefaultToState(detectionFields)).toEqual({
      postprocess_profile: 'yolov8n',
      threshold: 0.5,
      nms_threshold: 0.45,
      max_detections: 64,
      labels: '',
    });
    expect(fieldDefaultToState([{ key: 'name', type: 'text' }])).toEqual({});
  });
});

describe('partitionFields', () => {
  it('routes postprocess fields to the output page and the rest to basic', () => {
    const { basic, postprocess } = partitionFields(detectionFields);
    expect(basic.map(f => f.key)).toEqual(['name']);
    expect(postprocess.map(f => f.key)).toEqual([
      'postprocess_profile',
      'threshold',
      'nms_threshold',
      'max_detections',
      'labels',
    ]);
  });
  it('preserves schema order within each group', () => {
    const shuffled: ModelFieldDef[] = [
      { key: 'labels', type: 'text' },
      { key: 'alpha', type: 'text' },
      { key: 'threshold', type: 'number' },
      { key: 'beta', type: 'text' },
    ];
    expect(partitionFields(shuffled).basic.map(f => f.key)).toEqual([
      'alpha',
      'beta',
    ]);
  });
});

describe('buildRegisterPreview', () => {
  it('projects the register payload shape with trimmed strings', () => {
    const preview = buildRegisterPreview({
      ...baseForm,
      modelId: '  spaced-id  ',
      variant: ' hailo_yolov8n ',
    });
    expect(preview).toEqual({
      model_id: 'spaced-id',
      model_type: 'detection',
      output_mode: 'platform',
      config: baseForm.config,
      model_variant: 'hailo_yolov8n',
    });
  });
  it('snapshots config so later edits do not mutate the preview', () => {
    const form: ModelImportFormState = {
      ...baseForm,
      config: { threshold: 0.5 },
    };
    const preview = buildRegisterPreview(form);
    form.config.threshold = 0.9;
    expect(preview.config).toEqual({ threshold: 0.5 });
  });
});

describe('validateModelForm', () => {
  it('accepts a fully populated form', () => {
    expect(validateModelForm(baseForm, baseCtx)).toEqual([]);
  });

  it('flags a missing model id on the basic_info section', () => {
    const issues = validateModelForm({ ...baseForm, modelId: '   ' }, baseCtx);
    expect(issues).toEqual([
      { field: 'modelId', section: 'basic_info', reason: 'required' },
    ]);
  });

  it.each([
    '-det', // leading dash
    'my model', // space
    '检测模型', // non-ascii
    'a'.repeat(65), // over 64 chars
  ])('rejects an out-of-charset model id %j', id => {
    const issues = validateModelForm({ ...baseForm, modelId: id }, baseCtx);
    expect(issues).toEqual([
      { field: 'modelId', section: 'basic_info', reason: 'model_id_invalid' },
    ]);
  });

  it('checks the charset before the duplicate-id lookup', () => {
    // '-det' is also present in the existing list; the charset gate must win.
    const ctx: ValidateModelFormCtx = {
      ...baseCtx,
      existingModelIds: new Set(['-det']),
    };
    const issues = validateModelForm({ ...baseForm, modelId: '-det' }, ctx);
    expect(issues).toEqual([
      { field: 'modelId', section: 'basic_info', reason: 'model_id_invalid' },
    ]);
  });

  it('flags a duplicate id only in create mode and only when the list is loaded', () => {
    const form = { ...baseForm, modelId: 'taken-id' };
    expect(validateModelForm(form, baseCtx)).toEqual([
      { field: 'modelId', section: 'basic_info', reason: 'model_id_exists' },
    ]);
    // update mode: the id is fixed, no duplicate check
    expect(validateModelForm(form, { ...baseCtx, isUpdate: true })).toEqual([]);
    // list not loaded yet: skip the client-side check
    expect(
      validateModelForm(form, { ...baseCtx, existingModelIds: null })
    ).toEqual([]);
  });

  it('is case-insensitive for the duplicate check', () => {
    expect(
      validateModelForm({ ...baseForm, modelId: 'TAKEN-ID' }, baseCtx)[0]
    ).toMatchObject({ reason: 'model_id_exists' });
  });

  it('flags a missing model type', () => {
    expect(
      validateModelForm({ ...baseForm, modelType: '' }, baseCtx)[0]
    ).toMatchObject({ field: 'modelType', section: 'basic_info' });
  });

  it('rejects platform mode when the HEF output is a feature map', () => {
    const issues = validateModelForm(baseForm, {
      ...baseCtx,
      platformModeDisabled: true,
    });
    expect(issues).toEqual([
      { field: 'outputMode', section: 'output', reason: 'output_mode_invalid' },
    ]);
    // raw mode is the fallback and must pass
    expect(
      validateModelForm(
        { ...baseForm, outputMode: 'raw' },
        { ...baseCtx, platformModeDisabled: true }
      )
    ).toEqual([]);
  });

  it('surfaces variant issues on the advanced section', () => {
    const issues = validateModelForm(
      {
        ...baseForm,
        variant: validVariant.replace('iou_threshold', 'iou_thresh'),
      },
      baseCtx
    );
    expect(issues).toEqual([
      expect.objectContaining({
        field: 'variant',
        section: 'advanced',
        reason: 'variant_missing_keys',
      }),
    ]);
  });

  it('flags a required dynamic field by config_<key>', () => {
    const issues = validateModelForm(
      { ...baseForm, config: { ...baseForm.config, postprocess_profile: '' } },
      baseCtx
    );
    expect(issues).toEqual([
      {
        field: 'config_postprocess_profile',
        section: 'output',
        reason: 'required',
      },
    ]);
  });

  it('rejects non-finite numbers', () => {
    const issues = validateModelForm(
      { ...baseForm, config: { ...baseForm.config, threshold: 'abc' } },
      baseCtx
    );
    expect(issues[0]).toMatchObject({
      field: 'config_threshold',
      reason: 'invalid_number',
    });
  });

  it('bounds threshold and nms_threshold to 0..1 even without schema min/max', () => {
    expect(
      validateModelForm(
        { ...baseForm, config: { ...baseForm.config, threshold: 1.2 } },
        baseCtx
      )[0]
    ).toMatchObject({ field: 'config_threshold', reason: 'threshold_range' });
    expect(
      validateModelForm(
        { ...baseForm, config: { ...baseForm.config, nms_threshold: -0.1 } },
        baseCtx
      )[0]
    ).toMatchObject({
      field: 'config_nms_threshold',
      reason: 'nms_threshold_range',
    });
  });

  it('honours schema min/max with params', () => {
    expect(
      validateModelForm(
        { ...baseForm, config: { ...baseForm.config, max_detections: 0 } },
        baseCtx
      )[0]
    ).toMatchObject({
      field: 'config_max_detections',
      section: 'output',
      reason: 'number_min',
      params: { min: 1 },
    });
    expect(
      validateModelForm(
        { ...baseForm, config: { ...baseForm.config, max_detections: 999 } },
        baseCtx
      )[0]
    ).toMatchObject({ reason: 'number_max', params: { max: 512 } });
  });

  it('skips optional numeric fields that are empty', () => {
    expect(
      validateModelForm(
        { ...baseForm, config: { ...baseForm.config, nms_threshold: '' } },
        baseCtx
      )
    ).toEqual([]);
  });

  it('attributes schema-field issues to the right section', () => {
    const issues = validateModelForm(
      {
        ...baseForm,
        modelType: '',
        config: { ...baseForm.config, name: '' },
      },
      baseCtx
    );
    const sections = issues.map(i => [i.field, i.section]);
    expect(sections).toContainEqual(['modelType', 'basic_info']);
    expect(sections).toContainEqual(['config_name', 'basic_info']);
    expect(sections.every(([, section]) => section === 'basic_info')).toBe(
      true
    );
  });

  it('emits issues in a stable order: id, type, variant, output mode, fields', () => {
    // With no type selected the variant is not checked at all — the
    // dispatcher mirrors the REST validator, which only validates variant
    // surfaces for detection/keypoint.
    const untyped = validateModelForm(
      {
        modelId: '',
        modelType: '',
        outputMode: 'platform',
        variant: '{oops',
        config: { threshold: 5 },
      },
      {
        ...baseCtx,
        platformModeDisabled: true,
        fields: [{ key: 'threshold', type: 'number', required: true }],
      }
    );
    expect(untyped.map(i => i.field)).toEqual([
      'modelId',
      'modelType',
      'outputMode',
      'config_threshold',
    ]);

    const issues = validateModelForm(
      {
        ...baseForm,
        modelId: '',
        variant: '{oops',
        outputMode: 'platform',
        config: { threshold: 5 },
      },
      // only threshold in scope so the ordering assertion sees one field issue
      {
        ...baseCtx,
        platformModeDisabled: true,
        fields: [{ key: 'threshold', type: 'number', required: true }],
      }
    );
    expect(issues.map(i => i.field)).toEqual([
      'modelId',
      'variant',
      'outputMode',
      'config_threshold',
    ]);
  });
});

describe('visibleSelectOptions', () => {
  const options = [
    { value: 'hailo_yolov8n_384_640', label: 'YOLOv8n 384x640 (default)' },
    { value: 'hailo_yolov8s_384_640', label: 'YOLOv8s 384x640' },
    {
      value: 'yolov5m_vehicles',
      label: 'YOLOv5m Vehicles 1920x1080',
      custom: true,
    },
  ];

  it('hides custom options that are not the active value', () => {
    expect(visibleSelectOptions(options, 'hailo_yolov8n_384_640')).toEqual(
      options.slice(0, 2)
    );
  });

  it('keeps the custom option when it is the active value', () => {
    expect(visibleSelectOptions(options, 'yolov5m_vehicles')).toEqual(options);
  });

  it('treats an undefined current value as nothing selected', () => {
    expect(visibleSelectOptions(options, undefined)).toEqual(
      options.slice(0, 2)
    );
  });
});

describe('suggestPostprocessProfile', () => {
  const options = [
    { value: 'hailo_yolov8n_384_640' },
    { value: 'hailo_yolov8s_384_640' },
    { value: 'yolov5m_vehicles', custom: true },
  ];

  it('matches a basename prefix in the vstream tensor names', () => {
    expect(
      suggestPostprocessProfile(
        options,
        'hailo_yolov8s_384_640/yolov8_nms_postprocess'
      )
    ).toBe('hailo_yolov8s_384_640');
  });

  it('matches the deployment-specific basename, surfacing the custom option', () => {
    expect(
      suggestPostprocessProfile(
        options,
        'yolov5m_vehicles/yolov5_nms_postprocess'
      )
    ).toBe('yolov5m_vehicles');
  });

  it('returns null without a prefix match or vstream info', () => {
    expect(suggestPostprocessProfile(options, 'conv21/conv22')).toBeNull();
    expect(suggestPostprocessProfile(options, undefined)).toBeNull();
    expect(suggestPostprocessProfile(options, '')).toBeNull();
  });

  it('does not match a basename appearing outside a tensor-name prefix', () => {
    // substring without the separating slash must not count as a match
    expect(
      suggestPostprocessProfile(options, 'yolov5m_vehicles_conv21')
    ).toBeNull();
  });
});

describe('prefillUpdateForm', () => {
  it('merges schema defaults, persisted config and promoted columns', () => {
    // The regression this guards: labels/nms_threshold live only in config,
    // threshold is promoted to a row column — all three must survive.
    const form = prefillUpdateForm(
      {
        model_id: 'yolov8n-demo',
        model_type: 'detection',
        output_mode: 'platform',
        variant: '',
        config: JSON.stringify({
          postprocess_profile: 'yolov8s',
          nms_threshold: 0.6,
          labels: 'person,car',
        }),
        threshold: 0.3,
      },
      detectionFields
    );

    expect(form.modelId).toBe('yolov8n-demo');
    expect(form.modelType).toBe('detection');
    expect(form.outputMode).toBe('platform');
    expect(form.config.postprocess_profile).toBe('yolov8s');
    expect(form.config.nms_threshold).toBe(0.6);
    expect(form.config.labels).toBe('person,car');
    // The promoted column wins over any config value.
    expect(form.config.threshold).toBe(0.3);
    // Fields absent from both config and columns fall back to defaults.
    expect(form.config.max_detections).toBe(64);
  });

  it('accepts an already-parsed config object', () => {
    const form = prefillUpdateForm(
      {
        model_id: 'pose',
        model_type: 'detection',
        config: { labels: ['a', 'b'] },
      },
      detectionFields
    );
    expect(form.config.labels).toEqual(['a', 'b']);
  });

  it('falls back to defaults + columns on corrupt config JSON', () => {
    const form = prefillUpdateForm(
      {
        model_id: 'm',
        model_type: 'detection',
        config: '{not json',
        threshold: 0.42,
      },
      detectionFields
    );
    expect(form.config.threshold).toBe(0.42);
    expect(form.config.postprocess_profile).toBe('yolov8n');
  });

  it('normalizes unknown output modes to platform and missing variant to empty', () => {
    const form = prefillUpdateForm(
      { model_id: 'm', model_type: 'detection' },
      detectionFields
    );
    expect(form.outputMode).toBe('platform');
    expect(form.variant).toBe('');

    const raw = prefillUpdateForm(
      { model_id: 'm', model_type: 'detection', output_mode: 'raw' },
      detectionFields
    );
    expect(raw.outputMode).toBe('raw');
  });
});

// Keypoint form schema as the backend capability endpoint ships it after the
// profile work: the profile select (always visible) plus two numeric
// controls restricted to the pose profile via ModelFieldDef.profiles.
const keypointFields: ModelFieldDef[] = [
  {
    key: 'postprocess_profile',
    type: 'select',
    required: true,
    default: 'facial_landmarks',
    options: [
      { value: 'facial_landmarks', label: 'Face landmarks' },
      { value: 'yolov8_pose', label: 'YOLOv8 pose' },
    ],
  },
  {
    key: 'threshold',
    type: 'number',
    default: 0.25,
    min: 0.01,
    max: 1,
    step: 0.01,
    profiles: ['yolov8_pose'],
  },
  {
    key: 'keypoint_threshold',
    type: 'number',
    default: 0.25,
    min: 0,
    max: 1,
    step: 0.01,
    profiles: ['yolov8_pose'],
  },
];

describe('filterFieldsByProfile', () => {
  it('hides profile-restricted fields when another profile is active', () => {
    const facial = filterFieldsByProfile(keypointFields, 'facial_landmarks');
    expect(facial.map(f => f.key)).toEqual(['postprocess_profile']);
  });

  it('shows profile-restricted fields when their profile is active', () => {
    const pose = filterFieldsByProfile(keypointFields, 'yolov8_pose');
    expect(pose.map(f => f.key)).toEqual([
      'postprocess_profile',
      'threshold',
      'keypoint_threshold',
    ]);
  });

  it('treats a missing profile as the facial default (legacy rows)', () => {
    expect(
      filterFieldsByProfile(keypointFields, undefined).map(f => f.key)
    ).toEqual(['postprocess_profile']);
    expect(filterFieldsByProfile(keypointFields, null).map(f => f.key)).toEqual(
      ['postprocess_profile']
    );
    expect(filterFieldsByProfile(keypointFields, '').map(f => f.key)).toEqual([
      'postprocess_profile',
    ]);
  });

  it('keeps profile-less fields for every type (detection is unaffected)', () => {
    for (const profile of [undefined, 'facial_landmarks', 'yolov8_pose']) {
      const keys = filterFieldsByProfile(detectionFields, profile).map(
        f => f.key
      );
      expect(keys).toEqual(detectionFields.map(f => f.key));
    }
  });
});

describe('suggestKeypointProfile', () => {
  it('maps pose-shaped vstreams to the pose decoder', () => {
    expect(
      suggestKeypointProfile('yolov8s_pose/conv21, yolov8s_pose/conv28')
    ).toBe('yolov8_pose');
  });

  it('maps face-shaped network names to the facial default', () => {
    expect(suggestKeypointProfile('', 'face_landmarks_lite')).toBe(
      'facial_landmarks'
    );
    expect(suggestKeypointProfile('landmarks/1: out')).toBe('facial_landmarks');
  });

  it('yields no opinion for identity-free or empty input', () => {
    expect(suggestKeypointProfile('', '')).toBeNull();
    expect(suggestKeypointProfile(undefined, undefined)).toBeNull();
    expect(
      suggestKeypointProfile('featurenet/1: out', 'featurenet')
    ).toBeNull();
  });
});

describe('parseVStreamTable', () => {
  const fixture = JSON.stringify({
    network_name: 'yolov8s_pose',
    vstreams: ['yolov8s_pose/conv21', 'yolov8s_pose/conv28'],
    input_width: 640,
    input_height: 640,
  });

  it('parses the HEF-info JSON into vstream rows', () => {
    const table = parseVStreamTable(fixture);
    expect(table).not.toBeNull();
    expect(table?.networkName).toBe('yolov8s_pose');
    expect(table?.vstreams).toEqual([
      'yolov8s_pose/conv21',
      'yolov8s_pose/conv28',
    ]);
  });

  it('falls back to raw_output lines when no vstreams array is present', () => {
    const table = parseVStreamTable(
      JSON.stringify({ network_name: 'x', raw_output: 'a/1\n\nb/2\n' })
    );
    expect(table?.vstreams).toEqual(['a/1', 'b/2']);
  });

  it('prefers the server-parsed output_vstreams array verbatim', () => {
    const table = parseVStreamTable(
      JSON.stringify({
        network_name: 'yolov8n',
        output_vstreams: ['yolov8n/conv21 (HailoStream) FLOAT32'],
        vstreams: ['Input VStream infos:', 'images (HailoStream) UINT8'],
      })
    );
    expect(table?.vstreams).toEqual(['yolov8n/conv21 (HailoStream) FLOAT32']);
  });

  it('direction-filters legacy vstreams arrays: inputs and headers never render as outputs (review 2026-09-21)', () => {
    const table = parseVStreamTable(
      JSON.stringify({
        network_name: 'yolov8n',
        vstreams: [
          'Output VStream infos:',
          'output_boxes (HailoStream) FLOAT32 [1, 8400, 84]',
          'Input VStream infos:',
          'images (HailoStream) UINT8, NHWC(1x720x1280x3)',
        ],
      })
    );
    expect(table?.vstreams).toEqual([
      'output_boxes (HailoStream) FLOAT32 [1, 8400, 84]',
    ]);
  });

  it('yields null when a legacy array carries no output rows at all', () => {
    const table = parseVStreamTable(
      JSON.stringify({
        network_name: 'x',
        vstreams: ['Input VStream infos:', 'images (HailoStream) UINT8'],
      })
    );
    expect(table).toBeNull();
  });

  it('yields null for empty, malformed, or shapeless input', () => {
    expect(parseVStreamTable('')).toBeNull();
    expect(parseVStreamTable(undefined)).toBeNull();
    expect(parseVStreamTable('not json at all')).toBeNull();
    expect(parseVStreamTable('[1,2,3]')).toBeNull();
    expect(parseVStreamTable(JSON.stringify({ network_name: 'x' }))).toBeNull();
    expect(parseVStreamTable(JSON.stringify({ vstreams: [] }))).toBeNull();
    expect(
      parseVStreamTable(JSON.stringify({ vstreams: [42, null] }))
    ).toBeNull();
  });
});

describe('outputVStreamRows', () => {
  it('extracts output rows from the inline Input/Output column layout', () => {
    const rows = outputVStreamRows([
      'VStream infos:',
      'Input  hailo_yolov8n_384_640/input_layer1 UINT8, NV12(192x640x3)',
      'Output hailo_yolov8n_384_640/yolov8_nms_postprocess FLOAT32, HAILO NMS BY CLASS(number of classes: 4)',
    ]);
    expect(rows).toEqual([
      'Output hailo_yolov8n_384_640/yolov8_nms_postprocess FLOAT32, HAILO NMS BY CLASS(number of classes: 4)',
    ]);
  });

  it('falls back to every non-empty line when no direction markers exist', () => {
    expect(outputVStreamRows(['a/1', '', 'b/2'])).toEqual(['a/1', 'b/2']);
  });
});

describe('effectivePostprocess', () => {
  it('raw delivery carries no postprocess payload, even with a blob variant', () => {
    expect(
      effectivePostprocess({
        modelType: 'detection',
        outputMode: 'raw',
        variant: '{"max_boxes":8}',
        config: { threshold: 0.5 },
      })
    ).toEqual({ source: 'none-raw', blob: null });
  });

  it('a custom blob wins verbatim over the composed config', () => {
    const out = effectivePostprocess({
      modelType: 'detection',
      outputMode: 'platform',
      variant: '  {"max_boxes": 8}',
      config: { threshold: 0.9, max_detections: 64 },
    });
    // Leading whitespace still counts as an override (load-time trim rule).
    expect(out.source).toBe('custom-blob');
    expect(out.blob).toEqual({ max_boxes: 8 });
    expect(out.rawVariant).toBe('{"max_boxes": 8}');
  });

  it('a corrupt stored blob reports custom-blob with a null blob', () => {
    const out = effectivePostprocess({
      modelType: 'keypoint',
      outputMode: 'platform',
      variant: '{"native_yolov8_pose":',
      config: {},
    });
    expect(out.source).toBe('custom-blob');
    expect(out.blob).toBeNull();
  });

  it('a bare detection name composes the blob the loader will submit', () => {
    // The loader's DetectionVariantJSON replaces every non-JSON detection
    // variant with the full composed blob (profile + stored thresholds), so
    // the panel shows the composed payload, not verbatim routing.
    const out = effectivePostprocess({
      modelType: 'detection',
      outputMode: 'platform',
      variant: 'hailo_yolov8n',
      config: { threshold: 0.5 },
    });
    expect(out.source).toBe('composed');
    expect(out.rawVariant).toBe('hailo_yolov8n');
    expect(out.blob).toMatchObject({ detection_threshold: 0.5 });
  });

  it('a bare non-detection name passes through as the routing name', () => {
    // Registration passes non-detection variants through unchanged —
    // only detection variants are rewritten by the loader.
    expect(
      effectivePostprocess({
        modelType: 'keypoint',
        outputMode: 'platform',
        variant: 'face_landmarks_custom',
        config: {},
      })
    ).toEqual({
      source: 'passthrough-name',
      blob: null,
      rawVariant: 'face_landmarks_custom',
    });
  });

  it('an empty variant composes from config + profile', () => {
    const out = effectivePostprocess({
      modelType: 'detection',
      outputMode: 'platform',
      variant: '',
      config: { threshold: 0.4, max_detections: 32 },
    });
    expect(out.source).toBe('composed');
    expect(out.blob).toMatchObject({
      detection_threshold: 0.4,
      max_boxes: 32,
    });
  });

  it('keypoint composes the pose blob, and facial composes nothing', () => {
    const pose = effectivePostprocess({
      modelType: 'keypoint',
      outputMode: 'platform',
      variant: '',
      config: { postprocess_profile: 'yolov8_pose', threshold: 0.6 },
      inputWidth: 640,
      inputHeight: 640,
    });
    expect(pose.source).toBe('composed');
    expect(pose.blob).toEqual({
      native_yolov8_pose: true,
      score_threshold: 0.6,
      keypoint_threshold: 0.25,
      yolov8_pose_network_width: 640,
      yolov8_pose_network_height: 640,
    });
    expect(
      effectivePostprocess({
        modelType: 'keypoint',
        outputMode: 'platform',
        variant: '',
        config: { postprocess_profile: 'facial_landmarks' },
      })
    ).toEqual({ source: 'none', blob: null });
  });
});

describe('withReplacementFileFacts', () => {
  it('returns the preview untouched without a replacement file', () => {
    const preview = buildRegisterPreview({
      modelId: 'm1',
      modelType: 'detection',
      outputMode: 'platform',
      variant: '',
      config: {},
    });
    expect(withReplacementFileFacts(preview, null)).toBe(preview);
    expect(withReplacementFileFacts(preview, undefined)).toBe(preview);
  });

  it('adds only defined file facts (diff rows vs the persisted payload)', () => {
    const preview = { model_id: 'm1' };
    expect(
      withReplacementFileFacts(preview, {
        file_hash: 'abc',
        file_size: 1024,
        network_name: 'yolov8n',
        vstream_info: '{}',
        // absent input dims must NOT surface (UpdateModel: explicit 0 = clear)
      })
    ).toEqual({
      model_id: 'm1',
      file_hash: 'abc',
      file_size: 1024,
      network_name: 'yolov8n',
      vstream_info: '{}',
    });
  });
});

describe('reloadRelevantChange', () => {
  const initial: ModelImportFormState = { ...baseForm };

  it('returns false without a snapshot (create mode)', () => {
    expect(reloadRelevantChange(null, baseForm)).toBe(false);
  });

  it('returns false when nothing reload-relevant moved', () => {
    expect(
      reloadRelevantChange(initial, {
        ...baseForm,
        config: { ...baseForm.config },
      })
    ).toBe(false);
    // A display-name edit never reaches the composed variant — no reload.
    expect(
      reloadRelevantChange(initial, {
        ...baseForm,
        config: { ...baseForm.config, name: 'renamed' },
      })
    ).toBe(false);
  });

  it('flags type, output-mode and variant changes', () => {
    expect(
      reloadRelevantChange(initial, { ...baseForm, modelType: 'keypoint' })
    ).toBe(true);
    expect(
      reloadRelevantChange(initial, { ...baseForm, outputMode: 'raw' })
    ).toBe(true);
    expect(
      reloadRelevantChange(initial, { ...baseForm, variant: 'hailo_yolov8n' })
    ).toBe(true);
  });

  it('flags config edits that change the composed template', () => {
    expect(
      reloadRelevantChange(initial, {
        ...baseForm,
        config: { ...baseForm.config, threshold: 0.9 },
      })
    ).toBe(true);
    expect(
      reloadRelevantChange(initial, {
        ...baseForm,
        config: {
          ...baseForm.config,
          postprocess_profile: 'hailo_yolov8s_384_640',
        },
      })
    ).toBe(true);
  });
});

describe('diffRegisterPreview', () => {
  it('flattens config rows and reports added/removed/changed keys', () => {
    const before = buildRegisterPreview(baseForm);
    const after = buildRegisterPreview({
      ...baseForm,
      modelId: 'yolov8n-demo-v2',
      config: {
        ...baseForm.config,
        threshold: 0.9,
        labels: 'person, face',
        max_detections: undefined,
      },
    });
    const rows = diffRegisterPreview(before, after);
    expect(rows).toContainEqual({
      key: 'model_id',
      before: 'yolov8n-demo',
      after: 'yolov8n-demo-v2',
    });
    expect(rows).toContainEqual({
      key: 'config.threshold',
      before: 0.5,
      after: 0.9,
    });
    expect(rows).toContainEqual({
      key: 'config.labels',
      before: 'person',
      after: 'person, face',
    });
    // Removed key: after value undefined.
    expect(rows).toContainEqual({
      key: 'config.max_detections',
      before: 64,
      after: undefined,
    });
    // Unchanged keys never appear.
    expect(rows.find(r => r.key === 'model_type')).toBeUndefined();
    expect(rows.find(r => r.key === 'config.nms_threshold')).toBeUndefined();
  });

  it('returns an empty diff for identical payloads', () => {
    expect(
      diffRegisterPreview(
        buildRegisterPreview(baseForm),
        buildRegisterPreview(baseForm)
      )
    ).toEqual([]);
  });
});

describe('field-dialect bridges', () => {
  it('maps detection field keys onto the plugin blob dialect', () => {
    expect(fieldKeyToVariantKey.threshold).toBe('detection_threshold');
    expect(fieldKeyToVariantKey.max_detections).toBe('max_boxes');
    expect(fieldKeyToVariantKey.nms_threshold).toBe('iou_threshold');
    expect(fieldKeyToVariantKey.labels).toBe('labels');
  });

  it('maps keypoint field keys onto the pose blob dialect', () => {
    expect(fieldKeyToPoseVariantKey.threshold).toBe('score_threshold');
    expect(fieldKeyToPoseVariantKey.keypoint_threshold).toBe(
      'keypoint_threshold'
    );
  });
});
