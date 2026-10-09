import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { ChevronDown, ChevronRight } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Label } from '@/components/ui/label';
import { Textarea } from '@/components/ui/textarea';

export interface AdvancedVariantSectionProps {
  variant: string;
  /** Active model type — the primer and hints differ between the detection
   * plugin schema and the keypoint pose blob (review 2026-09-21). */
  modelType: string;
  onChange: (value: string) => void;
  onBlur: () => void;
  /** live client-mirror validation text (null when the variant is valid). */
  liveErrorText: string | null;
  /** shell's insertVariantTemplate — snapshot current values as JSON. */
  onInsertTemplate: () => void;
  /** Whether a template exists for the current configuration — false under
   *  keypoint's facial profile (the decoder takes no configuration at all,
   *  so there is nothing to snapshot and the button would be a no-op). */
  canInsertTemplate: boolean;
  /** raw delivery: the variant only steers the postprocess plugin. */
  isRawMode: boolean;
  disabled: boolean;
}

/**
 * Advanced page of the configure screen — the custom variant JSON escape
 * hatch into the postprocess plugin's schema (detection) or the pose
 * decoder blob (keypoint). The intro line states what the field is; the
 * when/how primer folds behind a toggle (collapsed by default — this page
 * is an escape hatch most imports never visit). Validation feedback is
 * live (not touched-gated), matching the old variantErrorText behavior.
 */
export default function AdvancedVariantSection({
  variant,
  modelType,
  onChange,
  onBlur,
  liveErrorText,
  onInsertTemplate,
  canInsertTemplate,
  isRawMode,
  disabled,
}: AdvancedVariantSectionProps) {
  const { t } = useTranslation();
  const [primerOpen, setPrimerOpen] = useState(false);
  const isKeypoint = modelType === 'keypoint';

  return (
    <div>
      <h3 className="mb-4 text-base font-semibold text-foreground">
        {t('sys.ai_models.wizard.nav_advanced', 'Advanced')}
      </h3>
      {/* What/when/how primer — the intro line stays always visible (what the
          field is); the when/how detail folds away by default so the page
          reads as "one field + its hint" until the user asks for the story.
          Most imports never touch this field. */}
      <div className="mb-4 rounded-lg border bg-muted/30 px-4 py-3 text-xs">
        <p className="text-muted-foreground">
          {isKeypoint
            ? t(
                'sys.ai_models.wizard.variant_pose_intro',
                'The model variant (model_variant) overrides the pose blob the platform composes at load time. A hand-written {…} blob is passed through to the decoder verbatim.'
              )
            : t(
                'sys.ai_models.wizard.variant_intro',
                'The model variant (model_variant) overrides the platform postprocess defaults. It only affects how detections are decoded, never inference itself.'
              )}
        </p>
        <button
          type="button"
          onClick={() => setPrimerOpen(prev => !prev)}
          aria-expanded={primerOpen}
          className="mt-2 flex items-center gap-1.5 font-medium text-foreground transition-colors hover:text-primary"
        >
          {primerOpen ? (
            <ChevronDown className="h-3.5 w-3.5" />
          ) : (
            <ChevronRight className="h-3.5 w-3.5" />
          )}
          {t(
            'sys.ai_models.wizard.variant_primer_toggle',
            'When and how to use it'
          )}
        </button>
        {primerOpen && (
          <div className="mt-3 grid gap-4 sm:grid-cols-2">
            <div>
              <p className="mb-1.5 font-medium text-foreground">
                {t(
                  'sys.ai_models.wizard.variant_when_title',
                  'When you need it'
                )}
              </p>
              {isKeypoint ? (
                <ul className="list-disc space-y-1 pl-4 text-muted-foreground">
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_pose_when_1',
                      'Pinning thresholds in the blob instead of the form (a blob overrides the composed config entirely)'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_pose_when_2',
                      'Recording network dimensions that differ from the parsed HEF values (create-time keys; changing them later requires a reload)'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_pose_when_3',
                      'Passing extra decoder keys the form does not expose — unknown keys are forwarded as-is'
                    )}
                  </li>
                </ul>
              ) : (
                <ul className="list-disc space-y-1 pl-4 text-muted-foreground">
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_when_1',
                      'A custom-trained model whose class count or labels differ from the built-in yolov8 configuration (e.g. a 2-class fire/smoke model)'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_when_2',
                      'Tuning NMS thresholds or the box limit without recompiling the HEF'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_when_3',
                      'The built-in profile does not match the model’s tensor layout and a different postprocess function is required'
                    )}
                  </li>
                </ul>
              )}
            </div>
            <div>
              <p className="mb-1.5 font-medium text-foreground">
                {t(
                  'sys.ai_models.wizard.variant_how_title',
                  'How to fill it in'
                )}
              </p>
              {isKeypoint ? (
                <ul className="list-disc space-y-1 pl-4 text-muted-foreground">
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_pose_how_1',
                      'Leave it empty — the profile selection above composes the blob at load time'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_pose_how_2',
                      'Full override: press the button below to generate the pose blob from the current values, then edit it (bare names are rejected)'
                    )}
                  </li>
                </ul>
              ) : (
                <ul className="list-disc space-y-1 pl-4 text-muted-foreground">
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_how_plain',
                      'Type a built-in variant name (e.g. hailo_yolov8n)'
                    )}
                  </li>
                  <li>
                    {t(
                      'sys.ai_models.wizard.variant_how_json',
                      'Full override: press the button below to generate a JSON template from the current values, then edit it'
                    )}
                  </li>
                </ul>
              )}
              <p className="mt-2 text-muted-foreground">
                {isKeypoint
                  ? t(
                      'sys.ai_models.wizard.variant_pose_skip_hint',
                      'Facial-landmark models (468 points, fully compile-time fixed) and default pose setups: leave it empty.'
                    )
                  : t(
                      'sys.ai_models.wizard.variant_skip_hint',
                      'Standard yolov8n / yolov8s / yolov8m models: leave it empty.'
                    )}
              </p>
            </div>
          </div>
        )}
      </div>
      <div className="grid gap-2">
        <Label htmlFor="variant">
          {t('sys.ai_models.form.variant', 'Variant')}
        </Label>
        <p className="text-xs text-muted-foreground">
          {isKeypoint
            ? t(
                'sys.ai_models.form.variant_pose_hint',
                'Overrides the pose blob composed at load time. A {…} blob is passed to the decoder verbatim (loader control keys are rejected); leave empty to use the profile selection above'
              )
            : t(
                'sys.ai_models.form.variant_hint',
                'Overrides the composed postprocess config. A {…} blob must carry the full plugin schema; leave empty to use the profile selection above'
              )}
        </p>
        <Textarea
          id="variant"
          rows={5}
          value={variant}
          onChange={e => onChange(e.target.value)}
          onBlur={onBlur}
          placeholder={
            isKeypoint
              ? t(
                  'sys.ai_models.form.variant_pose_placeholder',
                  'Empty for the composed pose blob, or paste a custom JSON object'
                )
              : t(
                  'sys.ai_models.form.variant_placeholder',
                  'Empty for defaults, or paste a full custom JSON blob'
                )
          }
          className="font-mono text-xs"
          disabled={disabled || isRawMode}
        />
        {liveErrorText && (
          <p className="text-sm text-destructive">{liveErrorText}</p>
        )}
        <div>
          <Button
            type="button"
            variant="outline"
            size="sm"
            onClick={onInsertTemplate}
            disabled={disabled || isRawMode || !canInsertTemplate}
          >
            {t(
              'sys.ai_models.form.variant_template',
              'Insert template from current values'
            )}
          </Button>
          {!canInsertTemplate && isKeypoint && (
            <p className="mt-1.5 text-xs text-muted-foreground">
              {t(
                'sys.ai_models.form.variant_template_none_facial',
                'The facial-landmarks decoder is fully compile-time fixed — there is no blob to compose for this profile.'
              )}
            </p>
          )}
        </div>
      </div>
    </div>
  );
}
