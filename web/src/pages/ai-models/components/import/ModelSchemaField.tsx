import { useTranslation } from 'react-i18next';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import type { ModelFieldDef } from '@/hooks/useModels';
import { visibleSelectOptions } from '../../lib/modelImportFlow';

export interface ModelSchemaFieldProps {
  field: ModelFieldDef;
  value: unknown;
  /** config value change — `undefined` clears the key's effective value. */
  onChange: (key: string, value: unknown) => void;
  /** blur marks the field touched so its derived error becomes visible. */
  onBlur: (key: string) => void;
  /** resolved error text, already gated on touched by the shell. */
  error?: string;
  /** global busy state (parse/register pending). */
  disabled?: boolean;
  /** raw delivery: postprocess knobs are baked into the HEF at compile
   *  time — grey them out (labels stay editable as consumer metadata). */
  inert?: boolean;
}

/**
 * One schema-driven config field of the register form. Four render branches
 * (number / select / boolean / text), migrated verbatim from the old
 * dialog's renderField — validation is derived upstream, so blur only
 * marks the field touched.
 */
export default function ModelSchemaField({
  field,
  value,
  onChange,
  onBlur,
  error,
  disabled,
  inert,
}: ModelSchemaFieldProps) {
  const { t } = useTranslation();
  const ph = t('sys.ai_models.form.placeholder', 'Please enter');
  const label = t(`sys.ai_models.form.${field.key}`, field.key);

  switch (field.type) {
    case 'number': {
      const val = value !== undefined ? String(value) : '';
      const isBoundedRatio = ['threshold', 'nms_threshold'].includes(field.key);
      const effectiveMin = isBoundedRatio ? 0 : field.min;
      const effectiveMax = isBoundedRatio ? 1 : field.max;
      const effectiveStep = isBoundedRatio
        ? (field.step ?? 0.01)
        : (field.step ?? 1);
      return (
        <div className="grid gap-2">
          <Label htmlFor={field.key}>{label}</Label>
          <Input
            id={field.key}
            type="number"
            step={effectiveStep}
            min={effectiveMin}
            max={effectiveMax}
            value={val}
            onChange={e => {
              const text = e.target.value;
              const v = text === '' ? undefined : parseFloat(text);
              onChange(field.key, v);
            }}
            onBlur={() => onBlur(field.key)}
            placeholder={ph}
            disabled={disabled || inert}
          />
          {error && <p className="text-sm text-destructive">{error}</p>}
        </div>
      );
    }
    case 'select': {
      const val = String(value ?? '');
      const hint = t(`sys.ai_models.form.${field.key}_hint`, '');
      return (
        <div className="grid gap-2">
          <Label htmlFor={field.key}>{label}</Label>
          <Select
            value={val}
            onValueChange={v => onChange(field.key, v)}
            disabled={disabled || inert}
          >
            <SelectTrigger
              id={field.key}
              className={error ? 'border-destructive' : ''}
            >
              <SelectValue
                placeholder={t(
                  'sys.ai_models.form.select_type',
                  'Please select'
                )}
              />
            </SelectTrigger>
            <SelectContent>
              {visibleSelectOptions(field.options ?? [], value).map(opt => (
                <SelectItem key={opt.value} value={opt.value}>
                  {t(`sys.ai_models.form.${field.key}_${opt.value}`, opt.label)}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          {error && <p className="text-sm text-destructive">{error}</p>}
          {hint && <p className="text-xs text-muted-foreground">{hint}</p>}
        </div>
      );
    }
    case 'boolean': {
      return (
        <div className="flex items-center gap-2">
          <input
            type="checkbox"
            id={field.key}
            checked={Boolean(value)}
            onChange={e => onChange(field.key, e.target.checked)}
            disabled={disabled || inert}
            className="h-4 w-4"
          />
          <Label htmlFor={field.key} className="font-normal">
            {label}
          </Label>
        </div>
      );
    }
    case 'text': {
      const hint = t(`sys.ai_models.form.${field.key}_hint`, '');
      // Field-specific examples (labels → "smoke,fire") beat the generic
      // "Please enter" where the expected format is not obvious.
      const example = t(`sys.ai_models.form.${field.key}_placeholder`, '');
      return (
        <div className="grid gap-2 sm:col-span-2">
          <Label htmlFor={field.key}>{label}</Label>
          <Input
            id={field.key}
            type="text"
            value={String(value ?? '')}
            onChange={e => onChange(field.key, e.target.value)}
            placeholder={example || ph}
            disabled={disabled}
          />
          {error && <p className="text-sm text-destructive">{error}</p>}
          {hint && <p className="text-xs text-muted-foreground">{hint}</p>}
        </div>
      );
    }
    default:
      return null;
  }
}
