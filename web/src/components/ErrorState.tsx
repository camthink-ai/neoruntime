import { AlertTriangle, RefreshCw } from 'lucide-react';
import { useTranslation } from 'react-i18next';

import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';

interface ErrorStateProps {
  className?: string;
  title?: string;
  description?: string;
  actionText?: string;
  onAction?: () => void;
  actionLoading?: boolean;
}

export default function ErrorState({
  className,
  title,
  description,
  actionText,
  onAction,
  actionLoading = false,
}: ErrorStateProps) {
  const { t } = useTranslation();

  const resolvedTitle = title ?? t('common.error', 'Error');
  const showAction = !!onAction;

  return (
    <div
      className={cn(
        'flex flex-col items-center justify-center py-8 text-center',
        className
      )}
    >
      <div className="mb-3 flex h-10 w-10 items-center justify-center rounded-full text-destructive">
        <AlertTriangle className="h-8 w-8" />
      </div>
      <div className="text-sm font-semibold text-foreground">
        {resolvedTitle}
      </div>
      {description ? (
        <div className="mt-1 max-w-md text-xs leading-relaxed text-muted-foreground">
          {description}
        </div>
      ) : null}
      {showAction ? (
        <div className="mt-4">
          <Button
            type="button"
            variant="outline"
            onClick={onAction}
            disabled={actionLoading}
          >
            <RefreshCw
              className={cn(
                'mr-2 h-4 w-4',
                actionLoading ? 'animate-spin' : ''
              )}
            />
            {actionText
              ?? (actionLoading
                ? t('common.loading', 'Loading...')
                : t('common.retry', 'Retry'))}
          </Button>
        </div>
      ) : null}
    </div>
  );
}
