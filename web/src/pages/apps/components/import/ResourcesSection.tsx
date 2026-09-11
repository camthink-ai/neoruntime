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

/** backing 仍为 `N%`，输入框仅展示数字 0–100 */
function cpuPercentToInputValue(cpu: string | undefined): string {
  if (!cpu?.trim()) return '';
  const t = cpu.trim();
  const m = t.match(/^(\d{1,3})\s*%$/);
  if (m) {
    const n = Math.min(100, Math.max(0, parseInt(m[1], 10)));
    return String(n);
  }
  const plain = t.match(/^(\d{1,3})$/);
  if (plain) {
    const n = Math.min(100, Math.max(0, parseInt(plain[1], 10)));
    return String(n);
  }
  return '';
}

function inputDigitsToCpuPercent(raw: string): string {
  const digits = raw.replace(/\D/g, '');
  if (digits === '') return '0%';
  let n = parseInt(digits, 10);
  if (Number.isNaN(n) || n < 0) n = 0;
  if (n > 100) n = 100;
  return `${n}%`;
}

export interface ResourcesSectionProps {
  config: WizardConfig;
  onChange: (next: WizardConfig) => void;
}

/**
 * 资源分配 page of the paginated import form: CPU / memory limits only —
 * runtime options (autostart, restart policy) live on 高级配置 so the nav
 * label matches what the page configures.
 */
export default function ResourcesSection({
  config,
  onChange,
}: ResourcesSectionProps) {
  const { t } = useTranslation();

  return (
    <div className="space-y-6">
      <div className="grid grid-cols-1 gap-4 sm:grid-cols-2">
        <div>
          <Label>{t('sys.apps.import.cpu_limit')}</Label>
          <div className="mt-2 flex items-center gap-2">
            <Input
              inputMode="numeric"
              autoComplete="off"
              maxLength={3}
              placeholder="50"
              className="flex-1 min-w-0"
              value={cpuPercentToInputValue(config.resources?.cpu)}
              onChange={e => onChange({
                  ...config,
                  resources: {
                    ...config.resources!,
                    cpu: inputDigitsToCpuPercent(e.target.value),
                  },
                })}
            />
            <span className="shrink-0 text-sm text-muted-foreground tabular-nums">
              %
            </span>
          </div>
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
            <SelectTrigger className="mt-2">
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
        </div>
      </div>
    </div>
  );
}
