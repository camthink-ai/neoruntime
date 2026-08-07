import { useTranslation } from 'react-i18next';
import { Card } from '@/components/ui/card';
import { Badge } from '@/components/ui/badge';
import {
  Clock,
  AlertCircle,
  AlertTriangle,
  Info,
  Zap,
  Server,
} from 'lucide-react';
import { cn } from '@/lib/utils';
import type { EventLogEntry, EventLevel } from '@/types/event_log';

interface EventCardProps {
  entry: EventLogEntry;
  onClick?: () => void;
}

const levelConfig: Record<
  EventLevel,
  {
    icon: React.ComponentType<{ className?: string }>;
    color: string;
    bgColor: string;
    label: string;
  }
> = {
  critical: {
    icon: Zap,
    color: 'text-purple-600 dark:text-purple-400',
    bgColor: 'bg-purple-50 dark:bg-purple-950/20',
    label: 'sys.event_logs.levels.critical',
  },
  error: {
    icon: AlertCircle,
    color: 'text-red-600 dark:text-red-400',
    bgColor: 'bg-red-50 dark:bg-red-950/20',
    label: 'sys.event_logs.levels.error',
  },
  warning: {
    icon: AlertTriangle,
    color: 'text-amber-600 dark:text-amber-400',
    bgColor: 'bg-amber-50 dark:bg-amber-950/20',
    label: 'sys.event_logs.levels.warning',
  },
  info: {
    icon: Info,
    color: 'text-blue-600 dark:text-blue-400',
    bgColor: 'bg-blue-50 dark:bg-blue-950/20',
    label: 'sys.event_logs.levels.info',
  },
};

const categoryBadgeColors: Record<string, string> = {
  operation: 'bg-blue-100 text-blue-800 dark:bg-blue-900/30 dark:text-blue-300',
  system: 'bg-gray-100 text-gray-800 dark:bg-gray-900/30 dark:text-gray-300',
};

// Service name mapping for user-friendly display
const serviceNames: Record<string, string> = {
  'platform-api': 'sys.event_logs.sources.platform_api',
  'ai-runtime': 'sys.event_logs.sources.ai_runtime',
  'camera-daemon': 'sys.event_logs.sources.camera_daemon',
  'app-manager': 'sys.event_logs.sources.app_manager',
  'device-control': 'sys.event_logs.sources.device_control',
  'event-bus': 'sys.event_logs.sources.event_bus',
  systemd: 'sys.event_logs.sources.systemd',
  sshd: 'sys.event_logs.sources.sshd',
};

function getSourceDisplayName(
  source: string,
  t: (key: string, fallback: string) => string
): string {
  const key = serviceNames[source];
  if (key) {
    return t(key, source);
  }
  return source;
}

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

export default function EventCard({ entry, onClick }: EventCardProps) {
  const { t } = useTranslation();

  const config = levelConfig[entry.level] || levelConfig.info;
  const Icon = config.icon;
  const categoryLabel = t(
    `sys.event_logs.categories.${entry.category}`,
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
          <div className="flex items-center gap-2 mb-2 flex-wrap">
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
              <span className="text-xs text-muted-foreground flex items-center gap-1">
                <Server className="w-3 h-3" />
                {getSourceDisplayName(entry.source, t)}
              </span>
            )}
            {entry.user && (
              <span className="text-xs text-muted-foreground">
                {t('sys.event_logs.table.user', '用户')}: {entry.user}
              </span>
            )}
          </div>

          {/* Message */}
          <div
            className={cn(
              'text-sm font-medium text-foreground mb-1',
              config.color
            )}
          >
            {entry.message}
          </div>
        </div>
      </div>
    </Card>
  );
}
