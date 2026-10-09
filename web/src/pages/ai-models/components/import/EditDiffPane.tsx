import { useTranslation } from 'react-i18next';
import { ArrowRight, Plus, Minus } from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import {
  diffRegisterPreview,
  type PreviewDiffRow,
} from '../../lib/modelImportFlow';
import JsonPreviewPane from './JsonPreviewPane';

export interface EditDiffPaneProps {
  /** buildRegisterPreview(initialForm) — the payload as persisted today. */
  before: Record<string, unknown>;
  /** buildRegisterPreview(form) — the payload the submit would send. */
  after: Record<string, unknown>;
}

/** Humanized one-line rendering of a diff value: undefined reads as
 *  "(absent)", objects/arrays as compact JSON, long strings truncated by
 *  CSS (the row keeps the full text in title). */
function valueText(value: unknown): string {
  if (value === undefined) return '';
  if (value === null) return 'null';
  if (typeof value === 'string') return value === '' ? '""' : value;
  if (typeof value === 'object') return JSON.stringify(value);
  return String(value);
}

function DiffRow({ row }: { row: PreviewDiffRow }) {
  const added = row.before === undefined;
  const removed = row.after === undefined;
  return (
    <div className="flex items-start gap-2 py-1.5">
      {added ? (
        <Plus className="mt-0.5 h-3.5 w-3.5 shrink-0 text-emerald-500" />
      ) : removed ? (
        <Minus className="mt-0.5 h-3.5 w-3.5 shrink-0 text-destructive" />
      ) : (
        <ArrowRight className="mt-0.5 h-3.5 w-3.5 shrink-0 text-muted-foreground" />
      )}
      <code className="shrink-0 font-mono text-xs text-foreground">
        {row.key}
      </code>
      <span className="min-w-0 flex-1 truncate font-mono text-xs text-muted-foreground">
        <span title={valueText(row.before)}>{valueText(row.before)}</span>
        <span className="mx-1.5 text-border">→</span>
        <span
          className={`${
            removed ? 'text-destructive line-through' : 'text-foreground'
          }`}
          title={valueText(row.after)}
        >
          {valueText(row.after)}
        </span>
      </span>
    </div>
  );
}

/**
 * Edit mode's JSON view: what the submit would CHANGE, before → after,
 * followed by the full payload preview. The edit page's job is 安全地改变
 * — a changed-keys list means the user confirms a diff, not re-reads a
 * config table they just filled in. Falls through to the plain preview
 * unchanged when nothing differs (the copy-to-curl use case keeps working).
 */
export default function EditDiffPane({ before, after }: EditDiffPaneProps) {
  const { t } = useTranslation();
  const rows = diffRegisterPreview(before, after);

  return (
    <div className="space-y-4">
      {rows.length > 0 ? (
        <section>
          <div className="mb-2 flex items-center gap-2">
            <h4 className="text-sm font-medium text-foreground">
              {t('sys.ai_models.wizard.diff_title', '本次提交将变更')}
            </h4>
            <Badge variant="secondary" className="px-1.5 py-0 text-[10px]">
              {rows.length}
            </Badge>
          </div>
          <p className="mb-1 text-xs text-muted-foreground">
            {t(
              'sys.ai_models.wizard.diff_hint',
              '与当前已保存配置相比的键级差异；config 逐键展开。'
            )}
          </p>
          <div className="divide-y divide-border rounded-lg border bg-muted/30 px-3 py-1.5">
            {rows.map(row => (
              <DiffRow key={row.key} row={row} />
            ))}
          </div>
        </section>
      ) : (
        <p className="text-xs text-muted-foreground">
          {t('sys.ai_models.wizard.diff_empty', '与已保存配置一致，无变更。')}
        </p>
      )}
      <JsonPreviewPane preview={after} />
    </div>
  );
}
