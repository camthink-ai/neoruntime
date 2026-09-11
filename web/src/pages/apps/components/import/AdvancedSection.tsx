import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Checkbox } from '@/components/ui/checkbox';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Plus, X } from 'lucide-react';
import type { WizardConfig } from '@/services/types';
import { restartPoliciesFor } from '../../lib/formFieldOptions';

/** i18n labels for the preset restart policies; out-of-preset values from
 * the manifest render with their raw value (what the file says). */
const RESTART_LABELS: Record<string, { key: string; fallback: string }> = {
  no: { key: 'sys.apps.import.restart_no', fallback: 'No Restart' },
  'on-failure': {
    key: 'sys.apps.import.restart_on_failure',
    fallback: 'On Failure',
  },
  always: { key: 'sys.apps.import.restart_always', fallback: 'Always Restart' },
};

export interface AdvancedSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
}

/**
 * 高级配置 page of the paginated import form: env vars, volume mounts and
 * runtime options (autostart, restart policy) — behavior-level config that
 * does not belong to resource limits or permissions.
 */
export default function AdvancedSection({
  config,
  onChange,
}: AdvancedSectionProps) {
  const { t } = useTranslation();

  return (
    <div className="space-y-6">
      {/* Environment Variables */}
      <div>
        <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
          <Label className="text-base font-semibold">
            {t('sys.apps.import.env_vars', 'Environment Variables')}
          </Label>
          <Button
            variant="outline"
            size="sm"
            className="w-full shrink-0 sm:w-auto"
            onClick={() => onChange({
                ...config,
                env: [...(config.env || []), { name: '', value: '' }],
              })}
          >
            <Plus className="w-4 h-4 mr-1" />
            {t('common.add', 'Add')}
          </Button>
        </div>
        <div className="space-y-2 border rounded-lg p-4">
          {config.env?.map((env, index) => (
            <div
              key={index}
              className="flex flex-col gap-2 sm:flex-row sm:items-center"
            >
              <Input
                placeholder="NAME"
                value={env.name}
                onChange={e => {
                  const newEnv = [...(config.env || [])];
                  newEnv[index] = {
                    ...newEnv[index],
                    name: e.target.value,
                  };
                  onChange({ ...config, env: newEnv });
                }}
                className="flex-1"
              />
              <Input
                placeholder="value"
                value={env.value}
                onChange={e => {
                  const newEnv = [...(config.env || [])];
                  newEnv[index] = {
                    ...newEnv[index],
                    value: e.target.value,
                  };
                  onChange({ ...config, env: newEnv });
                }}
                className="flex-1"
              />
              <Button
                variant="ghost"
                size="icon"
                onClick={() => {
                  const newEnv = config.env?.filter((_, i) => i !== index);
                  onChange({ ...config, env: newEnv });
                }}
              >
                <X className="w-4 h-4" />
              </Button>
            </div>
          ))}
          {(!config.env || config.env.length === 0) && (
            <p className="text-sm text-muted-foreground py-2">
              {t('sys.apps.import.no_env_vars', 'No environment variables')}
            </p>
          )}
        </div>
      </div>

      {/* Volumes */}
      <div>
        <div className="mb-3 flex flex-col gap-2 sm:flex-row sm:items-center sm:justify-between">
          <Label className="text-base font-semibold">
            {t('sys.apps.import.volumes', 'Volume Mounts')}
          </Label>
          <Button
            variant="outline"
            size="sm"
            className="w-full shrink-0 sm:w-auto"
            onClick={() => onChange({
                ...config,
                volumes: [
                  ...(config.volumes || []),
                  { host: '', container: '', readonly: false },
                ],
              })}
          >
            <Plus className="w-4 h-4 mr-1" />
            {t('common.add', 'Add')}
          </Button>
        </div>
        <div className="space-y-2 border rounded-lg p-4">
          {config.volumes?.map((vol, index) => (
            <div
              key={index}
              className="flex flex-col gap-2 sm:flex-row sm:items-center"
            >
              <Input
                placeholder={t('sys.apps.import.host_path', 'Host Path')}
                value={vol.host}
                onChange={e => {
                  const newVols = [...(config.volumes || [])];
                  newVols[index] = {
                    ...newVols[index],
                    host: e.target.value,
                  };
                  onChange({ ...config, volumes: newVols });
                }}
                className="flex-1"
              />
              <Input
                placeholder={t('sys.apps.import.container_path', '容器路径')}
                value={vol.container}
                onChange={e => {
                  const newVols = [...(config.volumes || [])];
                  newVols[index] = {
                    ...newVols[index],
                    container: e.target.value,
                  };
                  onChange({ ...config, volumes: newVols });
                }}
                className="flex-1"
              />
              <label className="flex items-center space-x-1 text-sm whitespace-nowrap">
                <Checkbox
                  checked={vol.readonly}
                  onCheckedChange={checked => {
                    const newVols = [...(config.volumes || [])];
                    newVols[index] = {
                      ...newVols[index],
                      readonly: !!checked,
                    };
                    onChange({ ...config, volumes: newVols });
                  }}
                />
                <span>RO</span>
              </label>
              <Button
                variant="ghost"
                size="icon"
                onClick={() => {
                  const newVols = config.volumes?.filter((_, i) => i !== index);
                  onChange({ ...config, volumes: newVols });
                }}
              >
                <X className="w-4 h-4" />
              </Button>
            </div>
          ))}
          {(!config.volumes || config.volumes.length === 0) && (
            <p className="text-sm text-muted-foreground py-2">
              {t('sys.apps.import.no_volumes', 'No volume mounts')}
            </p>
          )}
        </div>
      </div>

      {/* Runtime Options (moved from the resources page — they are
       * behavior, not resource limits) */}
      <div>
        <Label className="text-base font-semibold mb-3 block">
          {t('sys.apps.import.runtime_options', 'Runtime Options')}
        </Label>

        <div className="flex items-center space-x-2">
          <Checkbox
            checked={config.autostart}
            onCheckedChange={checked => onChange({ ...config, autostart: !!checked })}
          />
          <Label className="font-normal">
            {t('sys.apps.import.autostart', 'Auto Start')}
          </Label>
        </div>

        <div className="mt-6">
          <Label>{t('sys.apps.import.restart_policy', 'Restart Policy')}</Label>
          <Select
            value={config.restart_policy}
            onValueChange={value => onChange({ ...config, restart_policy: value })}
          >
            <SelectTrigger className="mt-2">
              <SelectValue />
            </SelectTrigger>
            <SelectContent>
              {restartPoliciesFor(config.restart_policy).map(opt => {
                const label = RESTART_LABELS[opt];
                return (
                  <SelectItem key={opt} value={opt}>
                    {label ? t(label.key, label.fallback) : opt}
                  </SelectItem>
                );
              })}
            </SelectContent>
          </Select>
        </div>
      </div>
    </div>
  );
}
