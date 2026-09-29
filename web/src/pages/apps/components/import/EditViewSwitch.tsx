import { useTranslation } from 'react-i18next';

export type EditView = 'form' | 'yaml';

export interface EditViewSwitchProps {
  view: EditView;
  /** Unapplied YAML edits exist — shows a dot on the YAML tab from either view. */
  yamlDirty: boolean;
  onChange: (view: EditView) => void;
}

/**
 * Segmented control that flips the import form's right column between the
 * form view and the YAML view. Deliberately its own module (not part of the
 * lazily loaded YAML pane) so the shell can import it eagerly without
 * dragging Monaco into the main chunk.
 */
export default function EditViewSwitch({
  view,
  yamlDirty,
  onChange,
}: EditViewSwitchProps) {
  const { t } = useTranslation();

  const item = (value: EditView, label: string, dot: boolean) => (
    <button
      type="button"
      onClick={() => onChange(value)}
      aria-pressed={view === value ? 'true' : undefined}
      className={`flex items-center gap-1.5 rounded-md px-3 py-1 text-sm transition-colors ${
        view === value
          ? 'bg-background font-medium text-foreground shadow-sm'
          : 'text-muted-foreground hover:text-foreground'
      }`}
    >
      {label}
      {dot && (
        <span
          className="h-1.5 w-1.5 rounded-full bg-amber-500"
          role="img"
          aria-label={t(
            'sys.apps.import.yaml_dirty_dot_aria',
            '有未应用的 YAML 修改'
          )}
        />
      )}
    </button>
  );

  return (
    <div className="flex items-center rounded-lg border border-border bg-muted/30 p-0.5">
      {item('form', t('sys.apps.import.form_view_tab', '表单'), false)}
      {item(
        'yaml',
        t('sys.apps.import.yaml_view_tab', 'YAML'),
        view === 'yaml' ? false : yamlDirty
      )}
    </div>
  );
}
