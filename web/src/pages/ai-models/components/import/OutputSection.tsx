import { useTranslation } from 'react-i18next';
import { Check, Info, Lock } from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Label } from '@/components/ui/label';
import type { ModelFieldDef } from '@/hooks/useModels';
import { POSTPROCESS_ONLY_FIELDS } from '../../lib/modelImportFlow';
import ModelSchemaField from './ModelSchemaField';

export interface OutputSectionProps {
  outputMode: string;
  onOutputModeChange: (value: string) => void;
  /** feature-map detection HEF: the plugin cannot decode it, card disables. */
  platformModeDisabled: boolean;
  /** schema fields routed to this page by partitionFields(). */
  postprocessFields: ModelFieldDef[];
  /** config snapshot from the form (field values keyed by schema key). */
  config: Record<string, unknown>;
  /** parsed NMS output vs a non-detection type — offer the one-click fix. */
  typeMismatch: boolean;
  onSwitchToDetection: () => void;
  /** update mode: postprocess_profile moved away from the loaded value. */
  profileChanged: boolean;
  disabled: boolean;
  /** resolved, touched-gated error text keyed 'outputMode' | `config_<key>`. */
  errorFor: (field: string) => string | undefined;
  onBlurField: (key: string) => void;
  onConfigChange: (key: string, value: unknown) => void;
}

/**
 * Output page of the configure screen, split into two labelled groups:
 * delivery-mode radio cards (compact — the one-line brief in the card, the
 * longer explanation as a dynamic line under the group) and the postprocess
 * parameters. In raw mode the compile-time-baked knobs move into a bordered,
 * lock-annotated reference box so it reads as "fixed in the HEF" instead of
 * a scattering of unexplained grey inputs; labels stay outside it as
 * consumer-side metadata.
 */
export default function OutputSection({
  outputMode,
  onOutputModeChange,
  platformModeDisabled,
  postprocessFields,
  config,
  typeMismatch,
  onSwitchToDetection,
  profileChanged,
  disabled,
  errorFor,
  onBlurField,
  onConfigChange,
}: OutputSectionProps) {
  const { t } = useTranslation();
  const isRaw = outputMode === 'raw';
  const outputModeError = errorFor('outputMode');

  const bakedFields = postprocessFields.filter(field => POSTPROCESS_ONLY_FIELDS.includes(field.key));
  const editableFields = postprocessFields.filter(
    field => !POSTPROCESS_ONLY_FIELDS.includes(field.key)
  );

  const renderField = (field: ModelFieldDef, inert: boolean) => (
    <ModelSchemaField
      key={field.key}
      field={field}
      value={config[field.key]}
      onChange={onConfigChange}
      onBlur={onBlurField}
      error={errorFor(`config_${field.key}`)}
      disabled={disabled}
      inert={inert}
    />
  );

  const radioCard = (
    value: 'platform' | 'raw',
    checked: boolean,
    cardDisabled: boolean,
    title: string,
    brief: string,
    reason?: string,
    recommended?: boolean
  ) => (
    <button
      type="button"
      role="radio"
      aria-checked={checked}
      disabled={disabled || cardDisabled}
      onClick={() => onOutputModeChange(value)}
      className={`rounded-lg border p-3 text-left transition-colors ${
        cardDisabled
          ? 'cursor-not-allowed border-border opacity-50'
          : checked
            ? 'border-primary bg-primary/5'
            : 'border-border hover:border-primary/50'
      }`}
    >
      <div className="flex items-center gap-2 text-sm font-medium">
        {checked && <Check className="h-4 w-4 shrink-0 text-primary" />}
        {title}
        {recommended && !cardDisabled && (
          <Badge
            variant="secondary"
            className="ml-auto px-1.5 py-0 text-[10px] leading-4"
          >
            {t('sys.ai_models.wizard.mode_recommended', 'Recommended')}
          </Badge>
        )}
      </div>
      <p className="mt-1 text-xs text-muted-foreground">{reason ?? brief}</p>
    </button>
  );

  return (
    <div className="space-y-5">
      <h3 className="text-base font-semibold text-foreground">
        {t('sys.ai_models.wizard.nav_output', 'Output & Postprocess')}
      </h3>

      {/* Group 1 — output delivery mode. The semantic type says what the
          outputs mean; this says how they are delivered (plugin-decoded
          structured results vs bare tensors for the consumer). Cards stay
          one-line-brief; the longer explanation follows dynamically. */}
      <section className="grid gap-2">
        <Label>
          {t('sys.ai_models.form.output_mode', 'Output Delivery Mode')} *
        </Label>
        <div
          role="radiogroup"
          aria-label={t(
            'sys.ai_models.form.output_mode',
            'Output Delivery Mode'
          )}
          className="grid grid-cols-1 gap-3 sm:grid-cols-2"
        >
          {radioCard(
            'platform',
            outputMode === 'platform',
            platformModeDisabled,
            t('sys.ai_models.form.output_mode_platform', 'Platform decode'),
            t(
              'sys.ai_models.wizard.output_mode_platform_brief',
              'Structured detections; thresholds tunable at runtime'
            ),
            platformModeDisabled
              ? t(
                  'sys.ai_models.form.output_mode_platform_disabled',
                  'HEF has no NMS output layer — the plugin cannot decode feature maps'
                )
              : undefined,
            true
          )}
          {radioCard(
            'raw',
            outputMode === 'raw',
            false,
            t('sys.ai_models.form.output_mode_raw', 'Raw tensors'),
            t(
              'sys.ai_models.wizard.output_mode_raw_brief',
              'Bare tensors; your app decodes them'
            )
          )}
        </div>
        {outputModeError && (
          <p className="text-sm text-destructive">{outputModeError}</p>
        )}
        <p className="text-xs text-muted-foreground">
          {isRaw
            ? t(
                'sys.ai_models.form.output_mode_raw_desc',
                'Inference returns bare output tensors; the consumer decodes them. Postprocess parameters are baked into the HEF at compile time'
              )
            : t(
                'sys.ai_models.form.output_mode_platform_desc',
                'Structured results via the postprocess plugin; thresholds and limits stay tunable at runtime'
              )}
        </p>
      </section>

      {typeMismatch && (
        <div className="flex items-start justify-between gap-2 rounded-md border border-blue-500/60 bg-blue-500/10 p-3 text-sm text-blue-700 dark:text-blue-400">
          <div className="flex items-start gap-2">
            <Info className="mt-0.5 h-4 w-4 shrink-0" />
            <span>
              {t(
                'sys.ai_models.wizard.type_mismatch',
                'This HEF carries the yolov8_nms_postprocess output layer but the type is not detection — it is likely miscategorized'
              )}
            </span>
          </div>
          <Button
            type="button"
            variant="outline"
            size="sm"
            onClick={onSwitchToDetection}
            disabled={disabled}
            className="shrink-0"
          >
            {t(
              'sys.ai_models.wizard.type_mismatch_switch',
              'Switch to detection'
            )}
          </Button>
        </div>
      )}

      {profileChanged && (
        <div className="flex items-start gap-2 rounded-md border border-blue-500/40 bg-blue-500/5 p-3 text-xs text-muted-foreground">
          <Info className="mt-0.5 h-4 w-4 shrink-0" />
          <span>
            {t(
              'sys.ai_models.wizard.profile_change_hint',
              'Changing the postprocess profile reloads the model if it is currently loaded'
            )}
          </span>
        </div>
      )}

      {/* Group 2 — postprocess parameters. Raw mode separates what is still
          editable (labels) from what was fixed at compile time. */}
      {postprocessFields.length > 0 && (
        <section className="grid gap-3">
          <div>
            <h4 className="text-sm font-medium text-foreground">
              {t(
                'sys.ai_models.wizard.output_params_title',
                'Postprocess Parameters'
              )}
            </h4>
            <p className="text-xs text-muted-foreground">
              {isRaw
                ? t(
                    'sys.ai_models.wizard.output_params_hint_raw',
                    'In raw mode the parameters below were baked into the model at compile time'
                  )
                : t(
                    'sys.ai_models.wizard.output_params_hint_platform',
                    'Thresholds and limits used by platform decode; adjustable after registration'
                  )}
            </p>
          </div>
          {isRaw ? (
            <>
              {editableFields.length > 0 && (
                <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                  {editableFields.map(field => renderField(field, false))}
                </div>
              )}
              {bakedFields.length > 0 && (
                <div className="rounded-md border bg-muted/40 p-3">
                  <p className="mb-3 flex items-start gap-2 text-xs text-muted-foreground">
                    <Lock className="mt-0.5 h-3.5 w-3.5 shrink-0" />
                    <span>
                      {t(
                        'sys.ai_models.wizard.baked_fields_hint',
                        'These values are baked into the HEF and shown for reference — changing them requires recompiling the model. Labels are consumer-side metadata and stay editable.'
                      )}
                    </span>
                  </p>
                  <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
                    {bakedFields.map(field => renderField(field, true))}
                  </div>
                </div>
              )}
            </>
          ) : (
            <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
              {postprocessFields.map(field => renderField(field, false))}
            </div>
          )}
        </section>
      )}
    </div>
  );
}
