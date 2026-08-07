import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import {
  ChevronRight,
  AlertTriangle,
  AlertCircle,
  FileText,
} from 'lucide-react';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Button } from '@/components/ui/button';
import { useQuery } from '@tanstack/react-query';
import { eventLogsApi } from '@/services/api/event_logs';
import type { EventLogEntry } from '@/types/event_log';

function formatTime(timestamp: string): string {
  const d = new Date(timestamp);
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}

function LevelIcon({ level }: { level: EventLogEntry['level'] }) {
  if (level === 'error' || level === 'critical') {
    return <AlertCircle className="w-3.5 h-3.5 text-destructive shrink-0" />;
  }
  return <AlertTriangle className="w-3.5 h-3.5 text-yellow-500 shrink-0" />;
}

function CategoryBadge({ category }: { category: EventLogEntry['category'] }) {
  const { t } = useTranslation();
  const isOperation = category === 'operation';
  return (
    <span
      className={`inline-flex items-center px-1.5 py-0.5 rounded text-[10px] font-medium shrink-0 ${
        isOperation
          ? 'bg-blue-500/10 text-blue-600 dark:text-blue-400'
          : 'bg-purple-500/10 text-purple-600 dark:text-purple-400'
      }`}
    >
      {isOperation
        ? t('sys.maintenance.operation_logs', '操作日志')
        : t('sys.maintenance.system_logs', '系统日志')}
    </span>
  );
}

export default function LogsCard() {
  const { t } = useTranslation();
  const navigate = useNavigate();

  const { data } = useQuery({
    queryKey: ['dashboard-alert-logs'],
    queryFn: async () => {
      const response = await eventLogsApi.list({ limit: 100 });
      return response.data;
    },
    refetchInterval: 30000,
    retry: false,
  });

  const entries = (data?.entries || [])
    .filter(
      e => e.level === 'warning' || e.level === 'error' || e.level === 'critical'
    )
    .slice(0, 20);

  return (
    <div className="bg-card rounded-2xl p-5 shadow-sm border border-border h-full flex flex-col">
      {/* Header */}
      <div className="flex items-center justify-between mb-4 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <FileText className="w-4 h-4 text-primary" />
          {t('sys.dashboard.alert_logs', '告警日志')}
        </h3>
        <Button
          variant="ghost"
          size="sm"
          className="text-xs text-muted-foreground hover:text-foreground h-7 px-2"
          onClick={() => navigate('/maintenance/logs')}
        >
          {t('common.view_all', '查看全部')}
          <ChevronRight className="w-3.5 h-3.5 ml-1" />
        </Button>
      </div>

      {/* List */}
      <ScrollArea className="flex-1 min-h-0">
        {entries.length === 0 ? (
          <div className="flex flex-col items-center justify-center h-full py-8 text-muted-foreground">
            <FileText className="w-10 h-10 mb-3 opacity-50" />
            <p className="text-sm">
              {t('sys.dashboard.no_alert_logs', '暂无告警日志')}
            </p>
          </div>
        ) : (
          <div className="space-y-1 pr-1">
            {entries.map(entry => (
              <div
                key={entry.id}
                className="flex items-start gap-2 px-2 py-2 rounded-lg hover:bg-secondary/50 transition-colors"
              >
                <LevelIcon level={entry.level} />
                <div className="flex-1 min-w-0">
                  <div className="flex items-center gap-1.5 flex-wrap mb-0.5">
                    <CategoryBadge category={entry.category} />
                    <span className="text-[11px] text-muted-foreground tabular-nums">
                      {formatTime(entry.timestamp)}
                    </span>
                  </div>
                  <p className="text-xs text-foreground leading-snug line-clamp-2">
                    {entry.message}
                  </p>
                </div>
              </div>
            ))}
          </div>
        )}
      </ScrollArea>
    </div>
  );
}
