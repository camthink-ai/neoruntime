import {
  forwardRef,
  useEffect,
  useImperativeHandle,
  useMemo,
  useState,
} from 'react';
import { useTranslation } from 'react-i18next';
import { AlertTriangle } from 'lucide-react';
import { useCapabilities, type ModelFieldDef } from '@/hooks/useModels';
import { useToast } from '@/hooks/use-toast';
import SectionNav from '@/pages/apps/components/import/SectionNav';
import {
  buildRegisterPreview,
  buildVariantTemplate,
  filterFieldsByProfile,
  mergeConfigOnTypeSwitch,
  modelFormIssueText,
  partitionFields,
  reloadRelevantChange,
  validateModelForm,
  variantFormIssue,
  variantOverridesProfile,
  withReplacementFileFacts,
  type ModelImportFormState,
  type ModelImportSectionId as SectionId,
  type ReplacementFileFacts,
} from '../../lib/modelImportFlow';
import BasicInfoSection from './BasicInfoSection';
import OutputSection from './OutputSection';
import AdvancedVariantSection from './AdvancedVariantSection';
import FormJsonSwitch, { type ModelFormView } from './FormJsonSwitch';
import JsonPreviewPane from './JsonPreviewPane';
import EditDiffPane from './EditDiffPane';
import RecognitionSummary from './RecognitionSummary';

/** Select-option shape of a capability model type (id + schema fields). */
export interface ModelTypeOption {
  value: string;
  label: string;
  fields: ModelFieldDef[];
  /** Per-decoder effect overrides (multi-decoder types only) — keys are the
   *  postprocess profile's select value, values are fieldKey → effect. */
  profileParamEffects?: Record<string, Record<string, ModelFieldDef['effect']>>;
}

/** Resolve capability model types into select options. Shared by the import
 *  wizard and the detail dialog's edit mode so both label identically. */
export function useModelTypeOptions(): ModelTypeOption[] {
  const { t } = useTranslation();
  const { data: capabilities } = useCapabilities();
  return useMemo(() => {
    if (!capabilities?.model_types) return [];
    return capabilities.model_types.map(mt => ({
      value: mt.id,
      label: t(`sys.ai_models.model_type.${mt.id}`, mt.label),
      fields: mt.fields,
      profileParamEffects: mt.profile_param_effects,
    }));
  }, [capabilities, t]);
}

export interface ModelConfigEditorHandle {
  /** Mark every field touched, validate, and hand back the current form —
   *  or null after toasting the first issue and jumping to its section. */
  submit: () => ModelImportFormState | null;
}

export interface ModelConfigEditorProps {
  /** Current form — the parent owns it (parse prefill / dirty checks / the
   *  register payload all read it); the editor patches it via onPatch. */
  form: ModelImportFormState;
  onPatch: (patch: Partial<ModelImportFormState>) => void;
  /** Update semantics: model id read-only, duplicate-id check skipped. */
  isUpdate: boolean;
  modelTypeOptions: ModelTypeOption[];
  /** Update mode: postprocess profile as loaded — changing it hints that the
   *  NPU reloads the model instead of silently keeping the old profile. */
  initialProfile?: string | null;
  /** Update mode: the form as prefilled from the stored row — drives the
   *  reload-impact banner and the edit-mode before→after diff view. Null
   *  in the import wizard (nothing is being changed yet). */
  initialForm?: ModelImportFormState | null;
  /** Server-classified output format ('nms' | 'feature_map' | '') from the
   *  parse result or, in update mode, the row's vstream info. */
  outputFormat: string;
  /** Parsed HEF's raw vstream info (HEF info JSON string) — feeds the raw
   *  tensor preview card; undefined in detail-page edit mode. */
  vstreamInfo?: string | null;
  /** Parsed HEF's suggested type — drives the miscategorized-type hint on a
   *  fresh import; undefined in detail-page edit mode (no parse involved). */
  suggestedType?: string;
  /** Parsed HEF's network dimensions — the keypoint variant template
   * records them (create-time keys); undefined in detail edit mode. */
  inputDims?: { width?: number; height?: number };
  /** Update mode with a replacement HEF selected: its file-identity facts
   * (hash/size/network/vstream/dims) are folded into the edit diff — the
   * replacement reloads the model even when every form field is unchanged,
   * and the persisted payload carries none of these keys. Null when no
   * new file was uploaded (metadata-only update). */
  replacementFile?: ReplacementFileFacts | null;
  /** Normalized ids of registered models; null = list not loaded yet. */
  existingModelIds?: Set<string> | null;
  disabled?: boolean;
  /** Mini-card above the section nav (the wizard passes what is being
   *  imported; the detail dialog omits it). */
  navHeader?: React.ReactNode;
}

/**
 * The configure screen shared by the import wizard and the detail dialog's
 * edit mode: section pagination + form/JSON flip + the three section pages,
 * plus the validation that gates submission. The form itself is controlled
 * by the parent; blur-gated error display and the active page/view are
 * editor-internal. Call submit() through the ref to run the submit gate
 * (touch everything, toast the first issue, jump to its section).
 */
const ModelConfigEditor = forwardRef<
  ModelConfigEditorHandle,
  ModelConfigEditorProps
>(
  (
    {
      form,
      onPatch,
      isUpdate,
      modelTypeOptions,
      initialProfile = null,
      initialForm = null,
      outputFormat,
      vstreamInfo,
      suggestedType,
      inputDims,
      replacementFile = null,
      existingModelIds = null,
      disabled = false,
      navHeader,
    },
    ref
  ) => {
    const { t } = useTranslation();
    const { toast } = useToast();
    const [activeSection, setActiveSection] = useState<SectionId>('basic_info');
    const [view, setView] = useState<ModelFormView>('form');
    // Only blur/submit marks survive here — error text is derived from the
    // form via validateModelForm, so it clears itself as values become valid.
    const [touched, setTouched] = useState<Record<string, boolean>>({});

    // Currently selected type's fields
    const currentOption = useMemo(
      () => modelTypeOptions.find(o => o.value === form.modelType),
      [modelTypeOptions, form.modelType]
    );
    const currentFields = useMemo(
      () => currentOption?.fields ?? [],
      [currentOption]
    );

    // Fields visible under the active postprocess profile. Profile-restricted
    // controls (keypoint thresholds exist only for yolov8_pose) are filtered
    // out of rendering, validation, and the submit-time touched map alike —
    // a hidden field must not block submission with an invisible error.
    // Effect badges are re-resolved per profile: multi-decoder types carry
    // per-decoder truth in profileParamEffects (yolov5m_vehicles reads none
    // of the knobs, so threshold shows "no effect" under it), while the
    // field-level annotation is the DEFAULT profile's truth.
    const activeProfile = form.config.postprocess_profile as string | undefined;
    const visibleFields = useMemo(() => {
      const shown = filterFieldsByProfile(currentFields, activeProfile);
      const profileEffects =        currentOption?.profileParamEffects?.[activeProfile ?? ''];
      if (!profileEffects) return shown;
      return shown.map(f => (profileEffects[f.key] ? { ...f, effect: profileEffects[f.key] } : f));
    }, [currentFields, currentOption, activeProfile]);

    // Platform decode requires the NMS output layer: a feature-map detection
    // HEF cannot enter the plugin pipeline at all, so the platform card is
    // disabled with the reason shown on the card itself.
    const platformModeDisabled =      form.modelType === 'detection' && outputFormat === 'feature_map';

    // When platform decode becomes impossible, fall through to raw instead of
    // sitting on an unregisterable selection.
    useEffect(() => {
      if (platformModeDisabled && form.outputMode === 'platform') {
        onPatch({ outputMode: 'raw' });
      }
    }, [platformModeDisabled, form.outputMode, onPatch]);

    // One validator drives everything: submit gates on issues[0] (toast +
    // jump to its section) and inline text is the same issue, gated on blur.
    const issues = useMemo(
      () => validateModelForm(form, {
          isUpdate,
          platformModeDisabled,
          existingModelIds,
          fields: visibleFields,
        }),
      [form, isUpdate, platformModeDisabled, existingModelIds, visibleFields]
    );

    const errorFor = (field: string): string | undefined => {
      if (!touched[field]) return undefined;
      const issue = issues.find(i => i.field === field);
      return issue ? modelFormIssueText(issue, t) : undefined;
    };

    // Live client mirror of the backend custom-variant guard, so a broken
    // blob is rejected before the request leaves the page (not blur-gated —
    // matches the old behavior under the textarea).
    const variantLiveText = useMemo(() => {
      const issue = variantFormIssue(form.variant, form.modelType);
      return issue ? modelFormIssueText(issue, t) : null;
    }, [form.variant, form.modelType, t]);

    // The inverse cross-check: the HEF ships the NMS output layer but is not
    // classified as detection — almost certainly miscategorized.
    const typeMismatch =      suggestedType !== undefined
      && suggestedType !== 'detection'
      && form.modelType !== 'detection'
      && outputFormat === 'nms';

    // Update mode: hint that changing the postprocess profile reloads a
    // loaded model rather than silently keeping the old profile on the NPU.
    const profileChanged =      isUpdate
      && initialProfile !== null
      && form.config.postprocess_profile !== undefined
      && form.config.postprocess_profile !== initialProfile;

    // The generalized reload truth: the REST update path reloads on
    // type/output-mode/variant changes, and config keys reach the runtime
    // only through the composed variant — so any composed difference
    // (threshold, profile, labels, dims…) reloads too. The profile-specific
    // hint in OutputSection stays as the contextual detail; this banner is
    // the page-level "your submit has a cost" signal (编辑=安全地改变).
    const reloadDirty = isUpdate && reloadRelevantChange(initialForm, form);

    const { basic: basicFields, postprocess: postprocessFields } = useMemo(
      () => partitionFields(visibleFields),
      [visibleFields]
    );

    const preview = useMemo(() => buildRegisterPreview(form), [form]);

    // Import wizard only: the parse's answer about this file, restated in
    // one card so confirming the recognition is a glance (导入=识别并建立).
    const showRecognition =      !isUpdate
      && (suggestedType !== undefined
        || inputDims?.width != null
        || inputDims?.height != null);

    // The advanced variant page exists only where a variant surface exists:
    // detection (closed plugin schema) and keypoint (pose blob escape
    // hatch). Other types' variants are passthrough strings the REST
    // dispatcher ignores, so the page would only invite editing a no-op.
    const supportsCustomVariant =      form.modelType === 'detection' || form.modelType === 'keypoint';

    const sections = useMemo(() => {
      const base: { id: SectionId; label: string }[] = [
        {
          id: 'basic_info',
          label: t('sys.ai_models.wizard.nav_basic_info', 'Basic Info'),
        },
        {
          id: 'output',
          label: t('sys.ai_models.wizard.nav_output', 'Output & Postprocess'),
        },
      ];
      if (supportsCustomVariant) {
        base.push({
          id: 'advanced',
          label: t('sys.ai_models.wizard.nav_advanced', 'Advanced'),
        });
      }
      return base;
    }, [t, supportsCustomVariant]);

    // A type switch away from detection/keypoint can leave the editor parked
    // on a page that no longer renders — fall back to the output page.
    useEffect(() => {
      if (!supportsCustomVariant && activeSection === 'advanced') {
        setActiveSection('output');
      }
    }, [supportsCustomVariant, activeSection]);

    const handleSectionChange = (id: string) => {
      // "Take me to that page": a nav click leaves the read-only JSON
      // projection — without this the pane stays on JsonPreviewPane and the
      // click looks dead (nothing to flush here, unlike apps' YAML view).
      setView('form');
      setActiveSection(id as SectionId);
    };

    const handleModelTypeChange = (value: string) => {
      const typeOpt = modelTypeOptions.find(o => o.value === value);
      onPatch({
        modelType: value,
        // New type's defaults overlaid with previously-entered values for
        // keys the new type also understands — keeps a tuned threshold alive
        // across a detection↔pose switch instead of silently wiping it.
        // Select values only survive when the new type offers them.
        config: mergeConfigOnTypeSwitch(form.config, typeOpt?.fields ?? []),
        // The variant speaks the OLD type's dialect (plugin schema vs pose
        // blob), and a `{`-blob wins over the profile at load time — a
        // stale variant would silently override the type just selected.
        variant: '',
      });
    };

    const updateConfig = (key: string, value: unknown) => {
      if (
        key === 'postprocess_profile'
        && value !== form.config.postprocess_profile
        && variantOverridesProfile(form.variant)
      ) {
        // A `{`-blob overrides the profile at load time (escape-hatch
        // precedence), so switching profile with a blob parked in the
        // advanced page would change nothing on the NPU. Clear it in the
        // same patch — the fresh profile selection takes over.
        onPatch({ config: { ...form.config, [key]: value }, variant: '' });
        return;
      }
      onPatch({ config: { ...form.config, [key]: value } });
    };

    // Seed the variant textarea with a schema-complete blob composed from the
    // visible form values, so the escape hatch starts from something the
    // postprocess plugin actually accepts (detection: the closed seven-key
    // schema; keypoint: the exact blob the load-time composer would send).
    // Null means this configuration has no variant surface to snapshot —
    // keypoint's facial profile composes nothing at all.
    const variantTemplate = useMemo(
      () => buildVariantTemplate({
          modelType: form.modelType,
          config: form.config,
          inputWidth: inputDims?.width,
          inputHeight: inputDims?.height,
        }),
      [form.modelType, form.config, inputDims?.width, inputDims?.height]
    );

    const insertVariantTemplate = () => {
      if (variantTemplate !== null) {
        onPatch({ variant: variantTemplate });
      }
    };

    useImperativeHandle(
      ref,
      () => ({
        submit: () => {
          const newTouched: Record<string, boolean> = {
            modelId: true,
            modelType: true,
            variant: true,
            outputMode: true,
          };
          for (const f of visibleFields) {
            newTouched[`config_${f.key}`] = true;
          }
          setTouched(newTouched);

          if (issues.length > 0) {
            toast({
              title: modelFormIssueText(issues[0], t),
              variant: 'destructive',
            });
            handleSectionChange(issues[0].section);
            return null;
          }
          return form;
        },
      }),
      [visibleFields, issues, form, toast, t]
    );

    return (
      <div className="flex min-h-0 flex-1 flex-col sm:flex-row">
        <SectionNav
          sections={sections}
          activeId={activeSection}
          onActiveChange={handleSectionChange}
          header={navHeader}
        />
        <div className="flex min-h-0 flex-1 flex-col">
          <div className="flex items-center justify-between gap-2 border-b border-border px-4 py-2 sm:px-6">
            <FormJsonSwitch view={view} onChange={setView} />
          </div>
          <div className="min-h-0 flex-1 overflow-y-auto px-4 py-5 sm:px-6 lg:px-8">
            {view === 'json' ? (
              isUpdate && initialForm ? (
                <EditDiffPane
                  before={buildRegisterPreview(initialForm)}
                  after={withReplacementFileFacts(preview, replacementFile)}
                />
              ) : (
                <JsonPreviewPane preview={preview} />
              )
            ) : (
              <>
                {showRecognition && (
                  <RecognitionSummary
                    suggestedType={suggestedType ?? ''}
                    chosenType={form.modelType}
                    chosenTypeLabel={currentOption?.label ?? form.modelType}
                    suggestedTypeLabel={
                      suggestedType
                        ? (modelTypeOptions.find(o => o.value === suggestedType)
                            ?.label ?? null)
                        : null
                    }
                    profile={
                      form.config.postprocess_profile as string | undefined
                    }
                    dims={inputDims ?? null}
                    outputFormat={outputFormat}
                  />
                )}
                {reloadDirty
                  && !(activeSection === 'output' && profileChanged) && (
                    <div className="mb-4 flex items-start gap-2 rounded-md border border-amber-500/60 bg-amber-500/10 p-3 text-xs text-amber-700 dark:text-amber-400">
                      <AlertTriangle className="mt-0.5 h-4 w-4 shrink-0" />
                      <span>
                        {t(
                          'sys.ai_models.wizard.reload_banner',
                          '提交后模型将重载（推理短暂中断）——本次修改改变了运行时收到的后处理载荷。'
                        )}
                      </span>
                    </div>
                  )}
                {activeSection === 'basic_info' ? (
                  <BasicInfoSection
                    form={form}
                    onModelIdChange={value => onPatch({ modelId: value })}
                    onModelTypeChange={handleModelTypeChange}
                    onBlurModelId={() => setTouched(prev => ({ ...prev, modelId: true }))}
                    modelTypeOptions={modelTypeOptions}
                    basicFields={basicFields}
                    isUpdate={isUpdate}
                    disabled={disabled}
                    errorFor={errorFor}
                    onBlurField={key => setTouched(prev => ({ ...prev, [`config_${key}`]: true }))}
                    onConfigChange={updateConfig}
                  />
                ) : activeSection === 'output' ? (
                  <OutputSection
                    outputMode={form.outputMode}
                    onOutputModeChange={value => onPatch({ outputMode: value })}
                    platformModeDisabled={platformModeDisabled}
                    postprocessFields={postprocessFields}
                    config={form.config}
                    typeMismatch={typeMismatch}
                    onSwitchToDetection={() => handleModelTypeChange('detection')}
                    profileChanged={profileChanged}
                    disabled={disabled}
                    errorFor={errorFor}
                    onBlurField={key => setTouched(prev => ({ ...prev, [`config_${key}`]: true }))}
                    onConfigChange={updateConfig}
                    vstreamInfo={vstreamInfo}
                  />
                ) : (
                  <AdvancedVariantSection
                    variant={form.variant}
                    modelType={form.modelType}
                    onChange={value => onPatch({ variant: value })}
                    onBlur={() => setTouched(prev => ({ ...prev, variant: true }))}
                    liveErrorText={variantLiveText}
                    onInsertTemplate={insertVariantTemplate}
                    canInsertTemplate={variantTemplate !== null}
                    isRawMode={form.outputMode === 'raw'}
                    disabled={disabled}
                  />
                )}
              </>
            )}
          </div>
        </div>
      </div>
    );
  }
);

ModelConfigEditor.displayName = 'ModelConfigEditor';

export default ModelConfigEditor;
