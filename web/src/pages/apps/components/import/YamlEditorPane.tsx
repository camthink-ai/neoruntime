import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { toast } from 'sonner';
import { DiffEditor } from '@monaco-editor/react';
import { Button } from '@/components/ui/button';
import {
  Copy,
  GitCompareArrows,
  Loader2,
  Check,
  CircleAlert,
} from 'lucide-react';
import MonacoEditor from '@/pages/maintenance/files/components/MonacoEditor';

export interface YamlEditorPaneProps {
  /** editable = a real uploaded app.yaml; preview = generated from the form. */
  mode: 'editable' | 'preview';
  value: string;
  /** The first uploaded file's text — baseline for the diff view. */
  originalValue?: string;
  dirty?: boolean;
  isFlushing?: boolean;
  /** Live parse/validation error on the current text, shown inline. */
  yamlError?: string | null;
  onChange?: (text: string) => void;
  /** Apply (= re-upload) the edited text; owned by the shell. */
  onApply?: () => void;
  /** Focus notifications — the shell suppresses form → text writes while
   * the editor is focused so live sync never rewrites under the cursor. */
  onFocusChange?: (focused: boolean) => void;
}

/**
 * YAML half of the import form's dual view. Editable mode edits the text of
 * the uploaded app.yaml in real-time sync with the form (the server stays
 * authoritative: edits are applied by re-uploading at view-leave/install);
 * preview mode shows the deterministic YAML the current form would
 * generate, read-only with copy. Loaded lazily so Monaco stays out of the
 * dialog's critical chunk.
 */
export default function YamlEditorPane({
  mode,
  value,
  originalValue,
  dirty = false,
  isFlushing = false,
  yamlError = null,
  onChange,
  onApply,
  onFocusChange,
}: YamlEditorPaneProps) {
  const { t } = useTranslation();
  const [showDiff, setShowDiff] = useState(false);

  const copyValue = async () => {
    try {
      await navigator.clipboard.writeText(value);
      toast.success(t('sys.apps.import.yaml_copied', '已复制到剪贴板'));
    } catch {
      toast.error(t('sys.apps.import.yaml_copy_failed', '复制失败'));
    }
  };

  const diffAvailable = mode === 'editable' && !!originalValue;

  return (
    <div
      className="flex min-h-0 flex-1 flex-col"
      onFocusCapture={() => onFocusChange?.(true)}
      onBlurCapture={() => onFocusChange?.(false)}
    >
      <div className="flex flex-wrap items-center justify-between gap-2 border-b border-border px-3 py-2 sm:px-4">
        <div className="flex flex-wrap items-center gap-2 text-xs">
          {mode === 'preview' ? (
            <span className="rounded-md bg-muted/60 px-2 py-1 text-muted-foreground">
              {t(
                'sys.apps.import.yaml_preview_hint',
                '只读预览 · 由表单生成，可复制后作为 app.yaml 使用'
              )}
            </span>
          ) : (
            <>
              <span
                className={`rounded-md px-2 py-1 ${
                  dirty
                    ? 'bg-amber-500/10 text-amber-600 dark:text-amber-400'
                    : 'bg-muted/60 text-muted-foreground'
                }`}
              >
                {dirty
                  ? t('sys.apps.import.yaml_dirty_chip', '有未应用的修改')
                  : t('sys.apps.import.yaml_clean_chip', '与文件一致')}
              </span>
              <span className="rounded-md bg-muted/60 px-2 py-1 text-muted-foreground">
                {t('sys.apps.import.yaml_live_sync_chip', '与表单实时同步')}
              </span>
            </>
          )}
        </div>

        <div className="flex items-center gap-1.5">
          {diffAvailable && (
            <Button
              variant="outline"
              size="sm"
              onClick={() => setShowDiff(s => !s)}
              aria-pressed={showDiff ? 'true' : undefined}
            >
              <GitCompareArrows className="h-4 w-4" />
              {t('sys.apps.import.yaml_diff_toggle', '对比原文件')}
            </Button>
          )}
          <Button variant="outline" size="sm" onClick={copyValue}>
            <Copy className="h-4 w-4" />
            {t('sys.apps.import.yaml_copy', '复制')}
          </Button>
          {mode === 'editable' && (
            <Button
              variant="carbon"
              size="sm"
              onClick={onApply}
              disabled={!dirty || isFlushing}
            >
              {isFlushing ? (
                <>
                  <Loader2 className="h-4 w-4 animate-spin" />
                  {t('sys.apps.import.yaml_flushing', '应用中…')}
                </>
              ) : (
                <>
                  <Check className="h-4 w-4" />
                  {t('sys.apps.import.yaml_apply', '应用')}
                </>
              )}
            </Button>
          )}
        </div>
      </div>

      {mode === 'editable' && yamlError && (
        <div
          role="alert"
          className="flex items-start gap-2 border-b border-destructive/30 bg-destructive/10 px-3 py-2 text-xs text-destructive sm:px-4"
        >
          <CircleAlert className="mt-0.5 h-3.5 w-3.5 shrink-0" />
          <span className="min-w-0 break-all">
            {t('sys.apps.import.yaml_parse_error', 'YAML 解析失败')}
            {': '}
            {yamlError}
          </span>
        </div>
      )}

      <div className="min-h-0 flex-1 overflow-hidden">
        {showDiff && diffAvailable ? (
          <DiffEditor
            height="100%"
            original={originalValue ?? ''}
            modified={value}
            language="yaml"
            theme="vs-dark"
            options={{
              readOnly: true,
              renderSideBySide: true,
              minimap: { enabled: false },
              automaticLayout: true,
              scrollBeyondLastLine: false,
              renderOverviewRuler: false,
            }}
          />
        ) : (
          <MonacoEditor
            value={value}
            language="yaml"
            readOnly={mode === 'preview' || isFlushing}
            onChange={onChange}
          />
        )}
      </div>
    </div>
  );
}
