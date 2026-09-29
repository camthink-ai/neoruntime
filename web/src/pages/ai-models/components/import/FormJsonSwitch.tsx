import { useTranslation } from 'react-i18next';

export type ModelFormView = 'form' | 'json';

export interface FormJsonSwitchProps {
  view: ModelFormView;
  onChange: (view: ModelFormView) => void;
}

/**
 * Segmented control flipping the configure screen between the form view and
 * the read-only JSON preview. Styled after apps' EditViewSwitch, minus the
 * dirty dot — the preview never accumulates unapplied edits.
 */
export default function FormJsonSwitch({
  view,
  onChange,
}: FormJsonSwitchProps) {
  const { t } = useTranslation();

  const item = (value: ModelFormView, label: string) => (
    <button
      type="button"
      onClick={() => onChange(value)}
      aria-pressed={view === value ? 'true' : undefined}
      className={`rounded-md px-3 py-1 text-sm transition-colors ${
        view === value
          ? 'bg-background font-medium text-foreground shadow-sm'
          : 'text-muted-foreground hover:text-foreground'
      }`}
    >
      {label}
    </button>
  );

  return (
    <div className="flex items-center rounded-lg border border-border bg-muted/30 p-0.5">
      {item('form', t('sys.ai_models.wizard.form_view_tab', 'Form'))}
      {item('json', t('sys.ai_models.wizard.json_view_tab', 'JSON'))}
    </div>
  );
}
