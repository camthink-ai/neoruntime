import { useTranslation } from 'react-i18next';
import type { InstallIssue, InstallIssueField } from '../../lib/importFlow';

export interface InlineValidationProps {
  issues: InstallIssue[];
  field: InstallIssueField;
}

/** Renders the authoritative pre-install checks beside the field they
 * describe. Errors block install; warnings use a softer amber treatment and
 * are confirmed once when the user starts installation. */
export default function InlineValidation({
  issues,
  field,
}: InlineValidationProps) {
  const { t } = useTranslation();
  const matching = issues.filter(issue => issue.field === field);
  if (matching.length === 0) return null;

  return (
    <div className="mt-1 space-y-1">
      {matching.map((issue, index) => (
        <p
          key={`${issue.reason}-${index}`}
          className={
            issue.severity === 'error'
              ? 'text-xs text-destructive'
              : 'text-xs text-amber-600 dark:text-amber-400'
          }
        >
          {t(`sys.apps.import.${issue.reason}`)}
        </p>
      ))}
    </div>
  );
}
