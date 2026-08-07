import { Pencil, Check, X } from 'lucide-react';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { useTranslation } from 'react-i18next';
import { cn } from '@/lib/utils';

export interface InfoGridItemProps {
  label: string;
  value?: string | null;
  isName?: boolean;
  editing?: boolean;
  editValue?: string;
  onEditChange?: (val: string) => void;
  onStartEdit?: () => void;
  onSave?: () => void;
  onCancel?: () => void;
  isSaving?: boolean;
  /** Mobile table: `<tr>`. Desktop grid: card cell. */
  layout?: 'stack' | 'table-row';
}

export function InfoGridItem({
  label,
  value,
  isName,
  editing,
  editValue,
  onEditChange,
  onStartEdit,
  onSave,
  onCancel,
  isSaving,
  layout = 'stack',
}: InfoGridItemProps) {
  const { t } = useTranslation();
  const displayValue =    value && value.trim().length > 0
      ? value
      : t('sys.common.unknown', 'Unknown');

  const editingRow = editing && isName && (
    <div
      className={cn(
        'flex flex-wrap items-center gap-1.5',
        layout === 'stack' ? 'w-full max-w-xs' : 'w-full'
      )}
    >
      <Input
        value={editValue}
        onChange={e => onEditChange?.(e.target.value)}
        className={cn(
          'h-8 min-w-0 bg-background text-xs',
          layout === 'table-row' && 'min-w-40 flex-1'
        )}
        autoFocus
      />
      <Button
        size="icon"
        variant="ghost"
        onClick={onSave}
        disabled={isSaving}
        className="h-7 w-7 shrink-0 bg-green-500/10 text-green-600 dark:text-green-500"
      >
        <Check className="w-3.5 h-3.5" />
      </Button>
      <Button
        size="icon"
        variant="ghost"
        onClick={onCancel}
        disabled={isSaving}
        className="h-7 w-7 shrink-0 bg-muted text-muted-foreground"
      >
        <X className="w-3.5 h-3.5" />
      </Button>
    </div>
  );

  const displayControls = !(editing && isName) && (
    <div
      className={cn(
        'flex gap-2 text-foreground',
        'min-h-8 items-center text-sm'
      )}
    >
      <span className="min-w-0 wrap-break-word whitespace-normal leading-snug">
        {displayValue}
      </span>
      {isName && (
        <Button
          size="icon"
          variant="ghost"
          className={cn(
            'h-6 w-6 shrink-0 text-muted-foreground',
            layout === 'stack' && 'ml-1'
          )}
          onClick={onStartEdit}
        >
          <Pencil className="w-3.5 h-3.5" />
        </Button>
      )}
    </div>
  );

  if (layout === 'table-row') {
    return (
      <tr>
        <td className="w-[38%] max-w-[42%] align-middle text-xs font-medium leading-snug text-muted-foreground">
          {label}
        </td>
        <td className="align-middle">{editingRow || displayControls}</td>
      </tr>
    );
  }

  return (
    <div className="flex flex-col gap-1">
      <div className="text-sm font-medium text-muted-foreground">{label}</div>
      {editingRow || displayControls}
    </div>
  );
}
