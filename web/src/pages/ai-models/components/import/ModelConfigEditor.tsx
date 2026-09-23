import {
  forwardRef,
  useEffect,
  useImperativeHandle,
  useMemo,
  useState,
} from 'react';
import { useTranslation } from 'react-i18next';
import { useCapabilities, type ModelFieldDef } from '@/hooks/useModels';
import { useToast } from '@/hooks/use-toast';
import SectionNav from '@/pages/apps/components/import/SectionNav';
import {
  backendFunctionForProfile,
  buildRegisterPreview,
  mergeConfigOnTypeSwitch,
  modelFormIssueText,
  partitionFields,
  validateModelForm,
  variantFormIssue,
  type ModelImportFormState,
  type ModelImportSectionId as SectionId,
} from '../../lib/modelImportFlow';
import BasicInfoSection from './BasicInfoSection';
import OutputSection from './OutputSection';
import AdvancedVariantSection from './AdvancedVariantSection';
import FormJsonSwitch, { type ModelFormView } from './FormJsonSwitch';
import JsonPreviewPane from './JsonPreviewPane';

/** Select-option shape of a capability model type (id + schema fields). */
export interface ModelTypeOption {
  value: string;
  label: string;
  fields: ModelFieldDef[];
}

/** Resolve capability model types into select options. Shared by the import
 *  wizard and the detail dialog's edit mode so both label identically. */
export function useModelTypeOptions(): ModelTypeOption[] {
  const { t } = useTranslation();
  const { data: capabilities } = useCapabilities();
  return useMemo(() => {
    if (!capabilities?.model_types) return [];
    return capabilities.model_types.map((mt) => ({
      value: mt.id,
      label: t(`sys.ai_models.model_type.${mt.id}`, mt.label),
      fields: mt.fields,
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
  /** Server-classified output format ('nms' | 'feature_map' | '') from the
   *  parse result or, in update mode, the row's vstream info. */
  outputFormat: string;
  /** Parsed HEF's suggested type — drives the miscategorized-type hint on a
   *  fresh import; undefined in detail-page edit mode (no parse involved). */
  suggestedType?: string;
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
      outputFormat,
      suggestedType,
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
    const currentFields = useMemo(() => {
      const opt = modelTypeOptions.find(o => o.value === form.modelType);
      return opt?.fields ?? [];
    }, [modelTypeOptions, form.modelType]);

    // Platform decode requires the NMS output layer: a feature-map detection
    // HEF cannot enter the plugin pipeline at all, so the platform card is
    // disabled with the reason shown on the card itself.
    const platformModeDisabled =    form.modelType === 'detection' && outputFormat === 'feature_map';

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
          fields: currentFields,
        }),
      [form, isUpdate, platformModeDisabled, existingModelIds, currentFields]
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
      const issue = variantFormIssue(form.variant);
      return issue ? modelFormIssueText(issue, t) : null;
    }, [form.variant, t]);

    // The inverse cross-check: the HEF ships the NMS output layer but is not
    // classified as detection — almost certainly miscategorized.
    const typeMismatch =    suggestedType !== undefined
      && suggestedType !== 'detection'
      && form.modelType !== 'detection'
      && outputFormat === 'nms';

    // Update mode: hint that changing the postprocess profile reloads a
    // loaded model rather than silently keeping the old profile on the NPU.
    const profileChanged =    isUpdate
      && initialProfile !== null
      && form.config.postprocess_profile !== undefined
      && form.config.postprocess_profile !== initialProfile;

    const { basic: basicFields, postprocess: postprocessFields } = useMemo(
      () => partitionFields(currentFields),
      [currentFields]
    );

    const preview = useMemo(() => buildRegisterPreview(form), [form]);

    const sections = useMemo(
      () => [
        {
          id: 'basic_info',
          label: t('sys.ai_models.wizard.nav_basic_info', 'Basic Info'),
        },
        {
          id: 'output',
          label: t('sys.ai_models.wizard.nav_output', 'Output & Postprocess'),
        },
        {
          id: 'advanced',
          label: t('sys.ai_models.wizard.nav_advanced', 'Advanced'),
        },
      ],
      [t]
    );

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
        config: mergeConfigOnTypeSwitch(form.config, typeOpt?.fields ?? []),
      });
    };

    const updateConfig = (key: string, value: unknown) => {
      onPatch({ config: { ...form.config, [key]: value } });
    };

    // Seed the variant textarea with a schema-complete blob composed from the
    // visible form values, so the escape hatch starts from something the
    // postprocess plugin actually accepts.
    const insertVariantTemplate = () => {
      const rawProfile = form.config.postprocess_profile;
      const fallbackProfile = 'hailo_yolov8n_384_640';
      const profile =      typeof rawProfile === 'string' ? rawProfile : fallbackProfile;
      const num = (v: unknown, fallback: number) => {
        if (typeof v !== 'number' || !Number.isFinite(v) || v <= 0) {
          return fallback;
        }
        return v;
      };
      const configLabels = form.config.labels;
      const rawLabels = typeof configLabels === 'string' ? configLabels : '';
      const labels = rawLabels
        .split(',')
        .map(s => s.trim())
        .filter(s => s !== '');
      const template = {
        backend_function: backendFunctionForProfile(profile),
        iou_threshold: num(form.config.nms_threshold, 0.45),
        detection_threshold: num(form.config.threshold, 0.25),
        output_activation: 'none',
        label_offset: 1,
        max_boxes: num(form.config.max_detections, 64),
        // Index 0 is a placeholder so labels[N] names class_id N.
        labels: ['unlabeled', ...labels],
      };
      onPatch({ variant: JSON.stringify(template, null, 2) });
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
          for (const f of currentFields) {
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
      [currentFields, issues, form, toast, t]
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
              <JsonPreviewPane preview={preview} />
            ) : activeSection === 'basic_info' ? (
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
              />
            ) : (
              <AdvancedVariantSection
                variant={form.variant}
                onChange={value => onPatch({ variant: value })}
                onBlur={() => setTouched(prev => ({ ...prev, variant: true }))}
                liveErrorText={variantLiveText}
                onInsertTemplate={insertVariantTemplate}
                isRawMode={form.outputMode === 'raw'}
                disabled={disabled}
              />
            )}
          </div>
        </div>
      </div>
    );
  }
);

ModelConfigEditor.displayName = 'ModelConfigEditor';

export default ModelConfigEditor;
