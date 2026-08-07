import { Check, Loader2, TriangleAlert } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import type { SaveStatus } from '../hooks/useSaveStatus';

interface SaveStatusIndicatorProps {
  status: SaveStatus;
}

export default function SaveStatusIndicator({
  status,
}: SaveStatusIndicatorProps) {
  const { t } = useTranslation();
  if (status === 'idle') return null;

  const content = {
    saving: {
      icon: <Loader2 aria-hidden="true" className="h-3 w-3 animate-spin" />,
      text: t('sys.common.saving', '正在保存…'),
      className: 'text-muted-foreground',
    },
    saved: {
      icon: <Check aria-hidden="true" className="h-3 w-3" />,
      text: t('sys.common.saved', '已保存'),
      className: 'text-emerald-600 dark:text-emerald-400',
    },
    error: {
      icon: <TriangleAlert aria-hidden="true" className="h-3 w-3" />,
      text: t('sys.common.save_failed_retry', '保存失败，已恢复设备配置'),
      className: 'text-destructive',
    },
  }[status];

  return (
    <div
      aria-live="polite"
      className={`flex min-h-5 items-center justify-end gap-1 text-xs ${content.className}`}
    >
      {content.icon}
      <span>{content.text}</span>
    </div>
  );
}
