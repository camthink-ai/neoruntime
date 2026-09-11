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
import type { ModelImportFormState } from '../../lib/modelImportFlow';
import ModelSchemaField from './ModelSchemaField';

export interface BasicInfoSectionProps {
  form: ModelImportFormState;
  onModelIdChange: (value: string) => void;
  onModelTypeChange: (value: string) => void;
  onBlurModelId: () => void;
  modelTypeOptions: { value: string; label: string }[];
  /** schema fields routed to this page by partitionFields(). */
  basicFields: ModelFieldDef[];
  /** update mode: the id is the model's immutable registry key. */
  isUpdate: boolean;
  disabled: boolean;
  /** resolved, touched-gated error text keyed 'modelId' | 'modelType' | `config_<key>`. */
  errorFor: (field: string) => string | undefined;
  onBlurField: (key: string) => void;
  onConfigChange: (key: string, value: unknown) => void;
}

/**
 * Basic-info page of the configure screen: model id (immutable in update
 * mode), model type, and the schema fields partitionFields() routes here.
 * Errors arrive pre-resolved from the shell's derived-issues map.
 */
export default function BasicInfoSection({
  form,
  onModelIdChange,
  onModelTypeChange,
  onBlurModelId,
  modelTypeOptions,
  basicFields,
  isUpdate,
  disabled,
  errorFor,
  onBlurField,
  onConfigChange,
}: BasicInfoSectionProps) {
  const { t } = useTranslation();
  const modelIdError = errorFor('modelId');
  const modelTypeError = errorFor('modelType');

  return (
    <div>
      <h3 className="mb-4 text-base font-semibold text-foreground">
        {t('sys.ai_models.wizard.nav_basic_info', 'Basic Info')}
      </h3>
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
        <div className="grid content-start gap-2">
          <Label htmlFor="modelId">
            {t('sys.ai_models.form.model_id', 'Model ID')}
          </Label>
          <Input
            id="modelId"
            type="text"
            value={form.modelId}
            onChange={e => onModelIdChange(e.target.value)}
            onBlur={onBlurModelId}
            placeholder={t('sys.ai_models.form.placeholder', 'Please enter')}
            readOnly={isUpdate}
            title={
              isUpdate
                ? t(
                    'sys.ai_models.form.model_id_readonly',
                    'Model ID cannot be changed after registration'
                  )
                : undefined
            }
            className={modelIdError ? 'border-destructive' : ''}
            disabled={disabled}
          />
          {modelIdError && (
            <p className="text-sm text-destructive">{modelIdError}</p>
          )}
        </div>

        <div className="grid content-start gap-2">
          <Label htmlFor="modelType">
            {t('sys.ai_models.form.model_type', 'Model Type')}
          </Label>
          <Select
            value={form.modelType}
            onValueChange={onModelTypeChange}
            disabled={disabled}
          >
            <SelectTrigger
              id="modelType"
              className={modelTypeError ? 'border-destructive' : ''}
            >
              <SelectValue
                placeholder={t(
                  'sys.ai_models.form.select_type',
                  'Please select'
                )}
              />
            </SelectTrigger>
            <SelectContent>
              {modelTypeOptions.map(opt => (
                <SelectItem key={opt.value} value={opt.value}>
                  {opt.label}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          {modelTypeError && (
            <p className="text-sm text-destructive">{modelTypeError}</p>
          )}
        </div>

        {basicFields.map(field => (
          <ModelSchemaField
            key={field.key}
            field={field}
            value={form.config[field.key]}
            onChange={onConfigChange}
            onBlur={onBlurField}
            error={errorFor(`config_${field.key}`)}
            disabled={disabled}
          />
        ))}
      </div>
    </div>
  );
}
