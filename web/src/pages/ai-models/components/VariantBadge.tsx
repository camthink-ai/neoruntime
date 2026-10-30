import { useTranslation } from 'react-i18next';
import {
  AlertTriangle,
  Braces,
  Layers3,
  SlidersHorizontal,
} from 'lucide-react';
import { Badge } from '@/components/ui/badge';
import { cn } from '@/lib/utils';
import { classifyModelVariant } from '../lib/variantDisplay';

interface VariantBadgeProps {
  variant?: string | null;
  /** Detail views can name the otherwise implicit platform-default state. */
  showDefault?: boolean;
  className?: string;
}

/** Compact semantic model-variant label; raw JSON belongs in the detail pane. */
export default function VariantBadge({
  variant,
  showDefault = false,
  className,
}: VariantBadgeProps) {
  const { t } = useTranslation();
  const display = classifyModelVariant(variant);
  if (display.kind === 'default' && !showDefault) return null;

  const content = {
    default: {
      icon: SlidersHorizontal,
      label: t('sys.ai_models.detail.variant_default', '平台默认'),
      style: 'text-muted-foreground',
    },
    preset: {
      icon: Layers3,
      label: t('sys.ai_models.detail.variant_preset', '预设 · {{name}}', {
        name: display.value,
      }),
      style:
        'border-blue-500/30 bg-blue-500/10 text-blue-700 dark:text-blue-300',
    },
    custom: {
      icon: Braces,
      label: t('sys.ai_models.detail.variant_custom', '自定义变体'),
      style:
        'border-violet-500/30 bg-violet-500/10 text-violet-700 dark:text-violet-300',
    },
    invalid: {
      icon: AlertTriangle,
      label: t('sys.ai_models.detail.variant_invalid', '变体配置异常'),
      style:
        'border-amber-500/40 bg-amber-500/10 text-amber-700 dark:text-amber-300',
    },
  }[display.kind];
  const Icon = content.icon;

  return (
    <Badge
      variant="outline"
      className={cn(
        'max-w-[180px] gap-1 rounded-full px-2 py-0.5 text-xs font-normal',
        content.style,
        className
      )}
      title={display.value || content.label}
    >
      <Icon className="h-3 w-3 shrink-0" />
      <span className="truncate">{content.label}</span>
    </Badge>
  );
}
