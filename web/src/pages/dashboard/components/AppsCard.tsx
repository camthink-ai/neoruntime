import { useState } from 'react';
import { useTranslation } from 'react-i18next';
import { useNavigate } from 'react-router-dom';
import {
  Play,
  Square,
  Loader2,
  ChevronRight,
  Package,
  ExternalLink,
} from 'lucide-react';
import { useMutation, useQueryClient } from '@tanstack/react-query';
import { appsApi } from '@/services/api';
import { Button } from '@/components/ui/button';
import { toast } from 'sonner';
import { ScrollArea } from '@/components/ui/scroll-area';
import { getAppWebUrl } from '@/pages/apps/lib/appWebUrl';

interface AppsCardProps {
  stats: {
    total: number;
    running: number;
    stopped: number;
    list: any[];
  };
}

function formatMemory(bytes: number): string {
  if (!bytes || bytes <= 0) return '0';
  const mb = bytes / 1024 / 1024;
  if (mb < 1) return `${Math.round(bytes / 1024)} KB`;
  if (mb >= 1024) return `${(mb / 1024).toFixed(1)} GB`;
  return `${mb.toFixed(1)} MB`;
}

export default function AppsCard({ stats }: AppsCardProps) {
  const { t } = useTranslation();
  const navigate = useNavigate();
  const queryClient = useQueryClient();
  const [pendingActions, setPendingActions] = useState<Set<string>>(new Set());

  const apps = (stats.list || [])
    .map((app: any) => ({
      id: app.id,
      name: app.name || app.id,
      version: app.version || '',
      state: app.state || app.status || 'stopped',
      web_url: app.web_url,
      permissions: app.permissions,
      cpu_percent: app.cpu_percent ?? 0,
      memory_usage: app.memory_usage || 0,
      memory_limit: app.memory_limit || 0,
      installed_at: app.installed_at,
      started_at: app.started_at,
      container_id: app.container_id,
    }))
    .sort((a, b) => {
      const aTime = a.installed_at || 0;
      const bTime = b.installed_at || 0;
      return bTime - aTime;
    });

  const runningApps = apps.filter(app => app.state === 'running');

  const startAppMutation = useMutation({
    mutationFn: (appId: string) => appsApi.start(appId),
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['dashboard', 'platformStats'],
      });
      toast.success(t('sys.apps.message.start_success', '应用启动成功'));
    },
    onError: () => {
      toast.error(t('sys.apps.message.start_failed', '应用启动失败'));
    },
    onSettled: (_data, _error, variables) => {
      setPendingActions(prev => {
        const next = new Set(prev);
        next.delete(`start-${variables}`);
        return next;
      });
    },
  });

  const stopAppMutation = useMutation({
    mutationFn: (appId: string) => appsApi.stop(appId),
    onSuccess: () => {
      queryClient.invalidateQueries({
        queryKey: ['dashboard', 'platformStats'],
      });
      toast.success(t('sys.apps.message.stop_success', '应用停止成功'));
    },
    onError: () => {
      toast.error(t('sys.apps.message.stop_failed', '应用停止失败'));
    },
    onSettled: (_data, _error, variables) => {
      setPendingActions(prev => {
        const next = new Set(prev);
        next.delete(`stop-${variables}`);
        return next;
      });
    },
  });

  const handleStart = (appId: string) => {
    setPendingActions(prev => new Set(prev).add(`start-${appId}`));
    startAppMutation.mutate(appId);
  };

  const handleStop = (appId: string) => {
    setPendingActions(prev => new Set(prev).add(`stop-${appId}`));
    stopAppMutation.mutate(appId);
  };

  const resourceSummary = runningApps.reduce(
    (acc, app) => ({
      totalCpu: acc.totalCpu + app.cpu_percent,
      totalMemory: acc.totalMemory + app.memory_usage,
    }),
    { totalCpu: 0, totalMemory: 0 }
  );

  function getStatusText(state: string) {
    switch (state) {
      case 'installed':
        return t('sys.apps.status.installed', '已安装');
      case 'running':
        return t('sys.apps.status.running', '运行中');
      case 'stopped':
        return t('sys.apps.status.stopped', '已停止');
      case 'failed':
        return t('sys.apps.status.failed', '失败');
      default:
        return t('sys.apps.status.unknown', '未知');
    }
  }

  function getStatusColor(state: string) {
    switch (state) {
      case 'running':
        return 'text-emerald-500';
      case 'stopped':
        return 'text-orange-500';
      case 'failed':
        return 'text-destructive';
      default:
        return 'text-muted-foreground';
    }
  }

  function getStatusDotColor(state: string) {
    switch (state) {
      case 'running':
        return 'bg-emerald-500';
      case 'stopped':
        return 'bg-orange-500';
      case 'failed':
        return 'bg-destructive';
      default:
        return 'bg-muted-foreground';
    }
  }

  function AppListItem({ app }: { app: (typeof apps)[number] }) {
    const isRunning = app.state === 'running';
    const isLoading =      pendingActions.has(`start-${app.id}`)
      || pendingActions.has(`stop-${app.id}`);
    const webUrl = getAppWebUrl(app as any);

    const memoryDisplay =      app.memory_limit > 0
        ? `${formatMemory(app.memory_usage)} / ${formatMemory(app.memory_limit)}`
        : formatMemory(app.memory_usage);

    return (
      <div className="flex items-center py-2 px-3 rounded-lg hover:bg-secondary/50 transition-colors group gap-2">
        <span
          className={`w-2 h-2 rounded-full shrink-0 ${getStatusDotColor(app.state)}`}
        />

        <div className="flex-1 min-w-0">
          <span className="text-sm font-medium text-foreground truncate block">
            {app.name}
          </span>
        </div>

        <span
          className={`text-[11px] font-bold shrink-0 w-10 text-center ${getStatusColor(app.state)}`}
        >
          {getStatusText(app.state)}
        </span>

        <span className="hidden 2xl:inline text-[11px] font-medium tabular-nums shrink-0 text-foreground w-10 text-right">
          {app.cpu_percent.toFixed(1)}%
        </span>

        <span className="hidden 2xl:inline w-28 md:w-32 shrink-0 text-right text-[11px] tabular-nums text-muted-foreground whitespace-nowrap">
          {memoryDisplay}
        </span>

        <div className="w-7 flex justify-center shrink-0">
          {isRunning ? (
            <Button
              variant="ghost"
              size="icon"
              className="h-6 w-6 text-muted-foreground hover:text-foreground"
              onClick={e => {
                e.stopPropagation();
                handleStop(app.id);
              }}
              disabled={isLoading}
            >
              {isLoading ? (
                <Loader2 className="h-3 w-3 animate-spin" />
              ) : (
                <Square className="h-3 w-3" />
              )}
            </Button>
          ) : (
            <Button
              variant="ghost"
              size="icon"
              className="h-6 w-6 text-emerald-600 hover:text-emerald-700"
              onClick={e => {
                e.stopPropagation();
                handleStart(app.id);
              }}
              disabled={isLoading}
            >
              {isLoading ? (
                <Loader2 className="h-3 w-3 animate-spin" />
              ) : (
                <Play className="h-3 w-3 fill-current" />
              )}
            </Button>
          )}
        </div>

        <div className="w-7 flex justify-center shrink-0">
          {webUrl && (
            <Button
              variant="ghost"
              size="icon"
              className="h-6 w-6 text-muted-foreground hover:text-primary"
              onClick={e => {
                e.stopPropagation();
                window.open(webUrl, '_blank');
              }}
            >
              <ExternalLink className="h-3 w-3" />
            </Button>
          )}
        </div>
      </div>
    );
  }

  return (
    <div className="bg-card rounded-2xl p-6 shadow-sm border border-border h-full max-lg:max-h-80 flex flex-col overflow-hidden">
      {/* Title */}
      <div className="flex items-center justify-between mb-4 shrink-0">
        <h3 className="text-base font-bold text-foreground flex items-center gap-2">
          <Package className="w-4 h-4 text-primary" />
          {t('sys.apps.title', 'Applications')}
          {runningApps.length > 0 && (
            <span className="text-xs font-normal text-muted-foreground">
              ({runningApps.length} {t('common.running', 'running')})
            </span>
          )}
        </h3>
        <Button
          variant="ghost"
          size="sm"
          className="text-xs text-muted-foreground hover:text-foreground h-7 px-2"
          onClick={() => navigate('/apps')}
        >
          {t('common.view_all', 'View all')}
          <ChevronRight className="w-3.5 h-3.5 ml-1" />
        </Button>
      </div>

      {/* List header */}
      <div className="flex items-center gap-2 px-3 py-2 text-[11px] text-muted-foreground border-b border-border shrink-0">
        <div className="w-2 shrink-0" />
        <div className="flex-1 min-w-0">{t('sys.apps.table.name')}</div>
        <div className="w-10 shrink-0 text-center">
          {t('sys.apps.table.status', 'Status')}
        </div>
        <div className="hidden 2xl:block w-10 shrink-0 text-right">CPU</div>
        <div className="hidden 2xl:block w-28 md:w-32 shrink-0 text-right whitespace-nowrap">
          {t('sys.apps.table.memory_usage')}
        </div>
        <div className="w-7 shrink-0 text-right">
          {t('common.actions', '操作')}
        </div>
        <div className="w-7 shrink-0" />
      </div>

      {/* App list */}
      <ScrollArea className="flex-1 min-h-0">
        {apps.length === 0 ? (
          <div className="flex flex-col items-center justify-center h-full py-8 text-muted-foreground">
            <Package className="w-10 h-10 mb-3 opacity-50" />
            <p className="text-sm">
              {t('sys.apps.no_apps', 'No apps installed')}
            </p>
            <Button
              variant="outline"
              size="sm"
              className="mt-3 h-8 text-xs"
              onClick={() => navigate('/apps')}
            >
              {t('sys.apps.action.install', 'Install App')}
            </Button>
          </div>
        ) : (
          <div className="py-1">
            {apps.map(app => (
              <AppListItem key={app.id} app={app} />
            ))}
          </div>
        )}
      </ScrollArea>

      {/* Resource summary */}
      {runningApps.length > 0 && (
        <div className="flex items-center justify-between pt-3 mt-3 border-t border-border text-xs text-muted-foreground flex-wrap gap-y-1 shrink-0">
          <span>{t('sys.apps.resource_summary', 'Resource Summary')}</span>
          <div className="flex items-center gap-3 sm:gap-4">
            <span>
              {t('sys.apps.total_cpu', 'Total CPU')}:{' '}
              <span className="font-medium text-foreground">
                {resourceSummary.totalCpu.toFixed(1)}%
              </span>
            </span>
            <span>
              {t('sys.apps.total_memory', 'Total Memory')}:{' '}
              <span className="font-medium text-foreground">
                {formatMemory(resourceSummary.totalMemory)}
              </span>
            </span>
          </div>
        </div>
      )}
    </div>
  );
}
