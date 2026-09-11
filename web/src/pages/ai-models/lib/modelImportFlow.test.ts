import { describe, expect, it } from 'vitest';
import type { ModelFieldDef } from '@/hooks/useModels';
import {
  backendFunctionForProfile,
  buildRegisterPreview,
  checkCustomVariant,
  classifyOutputFormat,
  fieldDefaultToState,
  formatFileSize,
  mergeConfigOnTypeSwitch,
  modelFormIssueText,
  MODEL_ID_PATTERN,
  partitionFields,
  prefillUpdateForm,
  sanitizeModelId,
  suggestModelId,
  suggestPostprocessProfile,
  validateModelForm,
  variantFormIssue,
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
    { key: 'postprocess_profile', type: 'select', default: 'yolov8n_pose' },
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
    expect(variantFormIssue('')).toBeNull();
    expect(variantFormIssue('{bad')).toMatchObject({
      field: 'variant',
      section: 'advanced',
      reason: 'variant_invalid_json',
    });
    const missing = variantFormIssue('{"backend_function":"hailo_yolov8n"}');
    expect(missing).toMatchObject({
      field: 'variant',
      section: 'advanced',
      reason: 'variant_missing_keys',
    });
    expect(missing?.params?.keys).toContain('iou_threshold');
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
    const issues = validateModelForm(
      {
        modelId: '',
        modelType: '',
        outputMode: 'platform',
        variant: '{oops',
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
      'modelType',
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
