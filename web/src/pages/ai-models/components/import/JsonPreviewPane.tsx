import { useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Check, Copy } from 'lucide-react';
import { Button } from '@/components/ui/button';

export interface JsonPreviewPaneProps {
  /** buildRegisterPreview(form) output — the exact register payload. */
  preview: Record<string, unknown>;
}

/** How long the copy button shows its confirmed state before reverting. */
const COPY_FEEDBACK_MS = 2000;

/**
 * Read-only projection of the register payload, refreshed live from the
 * form. One-way by design: schema keys are validated on input, so a free
 * JSON editor would mostly produce payloads the backend rejects — JSON-level
 * customization rides the advanced section's variant textarea instead. The
 * copy button exists because the pane doubles as "the exact JSON to send
 * via curl/CLI".
 */
export default function JsonPreviewPane({ preview }: JsonPreviewPaneProps) {
  const { t } = useTranslation();
  const [copied, setCopied] = useState(false);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  const handleCopy = async () => {
    try {
      await navigator.clipboard.writeText(JSON.stringify(preview, null, 2));
      setCopied(true);
      if (timerRef.current) clearTimeout(timerRef.current);
      timerRef.current = setTimeout(() => setCopied(false), COPY_FEEDBACK_MS);
    } catch {
      // Clipboard access denied (insecure context / permission) — the JSON
      // stays selectable for a manual copy; the button simply does nothing.
    }
  };

  return (
    <div className="space-y-3">
      <div className="flex items-start justify-between gap-2">
        <p className="text-xs text-muted-foreground">
          {t(
            'sys.ai_models.wizard.json_preview_hint',
            'Read-only preview of the payload that will be submitted. For JSON-level customization, edit the variant in the Advanced section.'
          )}
        </p>
        <Button
          type="button"
          variant="outline"
          size="sm"
          className="h-7 shrink-0 gap-1.5 px-2 text-xs"
          onClick={handleCopy}
          aria-label={t('common.copy', 'Copy')}
        >
          {copied ? (
            <Check className="h-3.5 w-3.5 text-emerald-500" />
          ) : (
            <Copy className="h-3.5 w-3.5" />
          )}
          {copied
            ? t('common.copied', 'Copied')
            : t('common.copy', 'Copy')}
        </Button>
      </div>
      <pre className="overflow-x-auto rounded-lg border border-border bg-muted/40 p-4 font-mono text-xs leading-relaxed text-foreground">
        {JSON.stringify(preview, null, 2)}
      </pre>
    </div>
  );
}
