import { useTranslation } from 'react-i18next';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import type { WizardConfig } from '@/services/types';

export interface BasicInfoSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
  /** app.yaml uploaded: the id binds the manifest directory and is immutable. */
  isIdReadOnly: boolean;
  existingAppIds: Set<string>;
}

/**
 * Basic info block of the one-page import form. Migrated verbatim from the
 * old wizard step 1 — only the id read-only condition is passed in instead
 * of reading `sourceType === 'package'` directly.
 */
export default function BasicInfoSection({
  config,
  onChange,
  isIdReadOnly,
  existingAppIds,
}: BasicInfoSectionProps) {
  const { t } = useTranslation();

  return (
    <div className="space-y-4">
      <div>
        <Label>{t('sys.apps.import.app_id')}</Label>
        <Input
          placeholder="my-app"
          value={config.metadata.id}
          onChange={e => onChange({
              ...config,
              metadata: { ...config.metadata, id: e.target.value },
            })}
          disabled={isIdReadOnly}
          className={`mt-2 ${existingAppIds.has(config.metadata.id.trim()) ? 'border-red-500 focus-visible:ring-red-500' : ''}`}
        />
        {isIdReadOnly && (
          <p className="mt-1 text-xs text-muted-foreground">
            {t('sys.apps.import.readonly_id_hint')}
          </p>
        )}
        {existingAppIds.has(config.metadata.id.trim()) && (
          <p className="mt-1 text-sm text-red-500">
            {t(
              'sys.apps.import.duplicate_id_warning',
              '此应用ID已存在，安装时将覆盖已有应用'
            )}
          </p>
        )}
      </div>

      <div>
        <Label>{t('sys.apps.import.app_name')}</Label>
        <Input
          placeholder="My Application"
          value={config.metadata.name}
          onChange={e => onChange({
              ...config,
              metadata: { ...config.metadata, name: e.target.value },
            })}
          className="mt-2"
        />
      </div>

      <div>
        <Label>{t('sys.apps.import.version')}</Label>
        <Input
          placeholder="1.0.0"
          value={config.metadata.version}
          onChange={e => onChange({
              ...config,
              metadata: { ...config.metadata, version: e.target.value },
            })}
          className="mt-2"
        />
      </div>

      <div>
        <Label>{t('sys.apps.import.description')}</Label>
        <Input
          placeholder="Application description"
          value={config.metadata.description}
          onChange={e => onChange({
              ...config,
              metadata: {
                ...config.metadata,
                description: e.target.value,
              },
            })}
          className="mt-2"
        />
      </div>
    </div>
  );
}
