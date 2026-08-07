import { useTranslation } from 'react-i18next';
import { Card } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import {
  Clock,
  AlertCircle,
  AlertTriangle,
  Info,
  Bug,
  Zap,
} from 'lucide-react';
import { cn } from '@/lib/utils';
import type { LogEntry, LogLevel } from '@/types/log';

interface LogCardProps {
  entry: LogEntry;
  onClick?: () => void;
}

const levelConfig: Record<
  LogLevel,
  {
    icon: React.ComponentType<{ className?: string }>;
    color: string;
    bgColor: string;
    label: string;
  }
> = {
  fatal: {
    icon: Zap,
    color: 'text-purple-600 dark:text-purple-400',
    bgColor: 'bg-purple-50 dark:bg-purple-950/20',
    label: 'sys.logs.levels.fatal',
  },
  error: {
    icon: AlertCircle,
    color: 'text-red-600 dark:text-red-400',
    bgColor: 'bg-red-50 dark:bg-red-950/20',
    label: 'sys.logs.levels.error',
  },
  warning: {
    icon: AlertTriangle,
    color: 'text-amber-600 dark:text-amber-400',
    bgColor: 'bg-amber-50 dark:bg-amber-950/20',
    label: 'sys.logs.levels.warning',
  },
  info: {
    icon: Info,
    color: 'text-blue-600 dark:text-blue-400',
    bgColor: 'bg-blue-50 dark:bg-blue-950/20',
    label: 'sys.logs.levels.info',
  },
  debug: {
    icon: Bug,
    color: 'text-gray-600 dark:text-gray-400',
    bgColor: 'bg-gray-50 dark:bg-gray-950/20',
    label: 'sys.logs.levels.debug',
  },
};

const categoryBadgeColors: Record<string, string> = {
  operation: 'bg-blue-100 text-blue-800 dark:bg-blue-900/30 dark:text-blue-300',
  security:
    'bg-purple-100 text-purple-800 dark:bg-purple-900/30 dark:text-purple-300',
  alarm: 'bg-red-100 text-red-800 dark:bg-red-900/30 dark:text-red-300',
  system: 'bg-gray-100 text-gray-800 dark:bg-gray-900/30 dark:text-gray-300',
};

function formatTimestamp(timestamp: string, t: any): string {
  const date = new Date(timestamp);
  const now = new Date();
  const diffMs = now.getTime() - date.getTime();
  const diffMins = Math.floor(diffMs / 60000);
  const diffHours = Math.floor(diffMs / 3600000);
  const diffDays = Math.floor(diffMs / 86400000);

  if (diffMins < 1) return t('sys.event_logs.time.just_now', '刚刚');
  if (diffMins < 60) {
 return t('sys.event_logs.time.minutes_ago', { m: diffMins } as Record<
      string,
      unknown
    >); 
}
  if (diffHours < 24) {
 return t('sys.event_logs.time.hours_ago', { h: diffHours } as Record<
      string,
      unknown
    >); 
}
  if (diffDays < 7) {
 return t('sys.event_logs.time.days_ago', { d: diffDays } as Record<
      string,
      unknown
    >); 
}

  return date.toLocaleString(t('sys.event_logs.time.locale', 'zh-CN'), {
    month: '2-digit',
    day: '2-digit',
    hour: '2-digit',
    minute: '2-digit',
  });
}

export default function LogCard({ entry, onClick }: LogCardProps) {
  const { t } = useTranslation();

  const config = levelConfig[entry.level] || levelConfig.info;
  const Icon = config.icon;
  const categoryLabel = t(
    `sys.logs.categories.${entry.category}`,
    entry.category
  );

  return (
    <Card
      className={cn(
        'p-4 border-l-4 cursor-pointer hover:shadow-md transition-shadow',
        config.bgColor,
        `border-l-${config.color.split('-')[1]}`
      )}
      onClick={onClick}
    >
      <div className="flex items-start gap-3">
        {/* Icon */}
        <div className={cn('p-2 rounded-lg', config.bgColor)}>
          <Icon className={cn('w-5 h-5', config.color)} />
        </div>

        {/* Content */}
        <div className="flex-1 min-w-0">
          {/* Header */}
          <div className="flex items-center gap-2 mb-2">
            <Badge
              variant="outline"
              className={categoryBadgeColors[entry.category]}
            >
              {categoryLabel}
            </Badge>
            <span className="text-xs text-muted-foreground flex items-center gap-1">
              <Clock className="w-3 h-3" />
              {formatTimestamp(entry.timestamp, t)}
            </span>
            {entry.source && (
              <span className="text-xs text-muted-foreground">
                · {entry.source}
              </span>
            )}
          </div>

          {/* Title */}
          <div
            className={cn(
              'text-sm font-medium text-foreground mb-1',
              config.color
            )}
          >
            {t(entry.title_key, entry.title)}
          </div>

          {/* Message */}
          <div className="text-sm text-muted-foreground line-clamp-2">
            {entry.message}
          </div>

          {/* Metadata */}
          {entry.metadata?.process && (
            <div className="mt-2 text-xs text-muted-foreground">
              {t('sys.logs.process', '进程')}: {entry.metadata.process}
              {entry.metadata.process_id && ` (${entry.metadata.process_id})`}
            </div>
          )}
        </div>
      </div>
    </Card>
  );
}
