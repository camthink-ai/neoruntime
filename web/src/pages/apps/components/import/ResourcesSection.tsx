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
import type { WizardConfig } from '@/services/types';
import { memoryOptionsFor } from '../../lib/formFieldOptions';
import type { InstallIssue } from '../../lib/importFlow';
import InlineValidation from './InlineValidation';

export interface ResourcesSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
  issues: InstallIssue[];
}

/**
 * 资源分配 page of the paginated import form: CPU / memory limits only —
 * runtime options (autostart, restart policy) live on 高级配置 so the nav
 * label matches what the page configures.
 */
export default function ResourcesSection({
  config,
  onChange,
  issues,
}: ResourcesSectionProps) {
  const { t } = useTranslation();

  return (
    <div className="space-y-6">
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
        <div>
          <Label>{t('sys.apps.import.cpu_limit')}</Label>
          <Input
            autoComplete="off"
            placeholder="0.5 / 50%"
            className="mt-2"
            value={config.resources?.cpu ?? ''}
            aria-invalid={
              issues.some(
                issue => issue.field === 'resources.cpu'
                  && issue.severity === 'error'
              ) || undefined
            }
            onChange={e => onChange({
                ...config,
                resources: {
                  ...config.resources!,
                  // Preserve the representation the user entered. Both core
                  // counts (0.5, 1.5) and percentages (50%) are valid manifest
                  // formats and are normalized only by the runtime quota parser.
                  cpu: e.target.value,
                },
              })}
          />
          <InlineValidation issues={issues} field="resources.cpu" />
        </div>

        <div>
          <Label>{t('sys.apps.import.memory_limit')}</Label>
          <Select
            value={config.resources?.memory}
            onValueChange={value => onChange({
                ...config,
                resources: { ...config.resources!, memory: value },
              })}
          >
            <SelectTrigger
              className="mt-2"
              aria-invalid={
                issues.some(
                  issue => issue.field === 'resources.memory'
                    && issue.severity === 'error'
                ) || undefined
              }
            >
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {memoryOptionsFor(config.resources?.memory).map(opt => (
                <SelectItem key={opt} value={opt}>
                  {opt}
                </SelectItem>
              ))}
            </SelectContent>
          </Select>
          <InlineValidation issues={issues} field="resources.memory" />
        </div>
      </div>
    </div>
  );
}
