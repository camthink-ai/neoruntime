import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Label } from '@/components/ui/label';
import { Textarea } from '@/components/ui/textarea';

export interface AdvancedVariantSectionProps {
  variant: string;
  onChange: (value: string) => void;
  onBlur: () => void;
  /** live client-mirror validation text (null when the variant is valid). */
  liveErrorText: string | null;
  /** shell's insertVariantTemplate — snapshot current values as JSON. */
  onInsertTemplate: () => void;
  /** raw delivery: the variant only steers the postprocess plugin. */
  isRawMode: boolean;
  disabled: boolean;
}

/**
 * Advanced page of the configure screen — the custom variant JSON escape
 * hatch into the postprocess plugin's schema. Opens with a what/when/how
 * primer so the field explains itself. The collapsible wrapper of the old
 * dialog is gone: being a nav page, it is open by construction. Validation
 * feedback is live (not touched-gated), matching the old variantErrorText
 * behavior.
 */
export default function AdvancedVariantSection({
  variant,
  onChange,
  onBlur,
  liveErrorText,
  onInsertTemplate,
  isRawMode,
  disabled,
}: AdvancedVariantSectionProps) {
  const { t } = useTranslation();

  return (
    <div>
      <h3 className="mb-4 text-base font-semibold text-foreground">
        {t('sys.ai_models.wizard.nav_advanced', 'Advanced')}
      </h3>
      {/* What/when/how primer — the variant field is the least self-evident
          knob in the flow, so the page opens by answering it before showing
          the input. Two columns keep it scannable at dialog width. */}
      <div className="mb-4 grid gap-4 rounded-lg border bg-muted/30 p-4 text-xs sm:grid-cols-2">
        <p className="text-muted-foreground sm:col-span-2">
          {t(
            'sys.ai_models.wizard.variant_intro',
            'The model variant (model_variant) overrides the platform postprocess defaults. It only affects how detections are decoded, never inference itself.'
          )}
        </p>
        <div>
          <p className="mb-1.5 font-medium text-foreground">
            {t('sys.ai_models.wizard.variant_when_title', 'When you need it')}
          </p>
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
        </div>
        <div>
          <p className="mb-1.5 font-medium text-foreground">
            {t('sys.ai_models.wizard.variant_how_title', 'How to fill it in')}
          </p>
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
          <p className="mt-2 text-muted-foreground">
            {t(
              'sys.ai_models.wizard.variant_skip_hint',
              'Standard yolov8n / yolov8s / yolov8m models: leave it empty.'
            )}
          </p>
        </div>
      </div>
      <div className="grid gap-2">
        <Label htmlFor="variant">
          {t('sys.ai_models.form.variant', 'Variant')}
        </Label>
        <p className="text-xs text-muted-foreground">
          {t(
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
          placeholder={t(
            'sys.ai_models.form.variant_placeholder',
            'Empty for defaults, or paste a full custom JSON blob'
          )}
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
            disabled={disabled || isRawMode}
          >
            {t(
              'sys.ai_models.form.variant_template',
              'Insert template from current values'
            )}
          </Button>
        </div>
      </div>
    </div>
  );
}
