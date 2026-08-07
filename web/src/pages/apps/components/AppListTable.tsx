import { useState } from 'react';
import React from 'react';
import { useTranslation } from 'react-i18next';
import { useIsMobile } from '@/hooks/use-mobile';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import {
  Square,
  RotateCw,
  Loader2,
  Terminal,
  FileText,
  Trash2,
  Play,
  ChevronLeft,
  ChevronRight,
  Video,
  Brain,
  Wifi,
  Lightbulb,
  Radio,
} from 'lucide-react';
import { Empty } from '@/components/ui/empty';
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover';
import { ScrollArea, ScrollBar } from '@/components/ui/scroll-area';
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip';
import { useAppStats } from '@/hooks';
import type { AppTemplate, AppPermissions } from '@/services/types';

function InlinePermissionIcons({
  permissions,
}: {
  permissions?: AppPermissions;
}) {
  if (!permissions) return null;
  const items: { icon: React.ReactNode; label: string; color: string }[] = [];
  if (permissions.video?.length) {
    items.push({
      icon: <Video className="w-3 h-3" />,
      label: permissions.video.join(', '),
      color: 'text-blue-500',
    });
  }
  if (permissions.inference?.models?.length) {
    items.push({
      icon: <Brain className="w-3 h-3" />,
      label: permissions.inference.models.join(', '),
      color: 'text-purple-500',
    });
  }
  if (
    (permissions.events?.publish?.length ?? 0) > 0
    || (permissions.events?.subscribe?.length ?? 0) > 0
  ) {
    items.push({
      icon: <Radio className="w-3 h-3" />,
      label: 'Events',
      color: 'text-amber-500',
    });
  }
  if (
    permissions.device?.light
    || permissions.device?.ir_cut
    || permissions.device?.ptz
    || permissions.device?.lens
  ) {
    items.push({
      icon: <Lightbulb className="w-3 h-3" />,
      label: [
        permissions.device?.light && 'Light',
        permissions.device?.ir_cut && 'IR-Cut',
        permissions.device?.ptz && 'PTZ',
        permissions.device?.lens && 'Lens',
      ]
        .filter(Boolean)
        .join(', '),
      color: 'text-orange-500',
    });
  }
  if (permissions.network?.mode) {
    items.push({
      icon: <Wifi className="w-3 h-3" />,
      label: permissions.network.mode,
      color: 'text-teal-500',
    });
  }
  if (items.length === 0) return null;

  return (
    <TooltipProvider delayDuration={200}>
      <div className="flex items-center gap-1 mt-1">
        {items.map((item, i) => (
          <Tooltip key={i}>
            <TooltipTrigger asChild>
              <span
                className={`inline-flex items-center justify-center w-5 h-5 rounded bg-muted/60 ${item.color} cursor-default`}
              >
                {item.icon}
              </span>
            </TooltipTrigger>
            <TooltipContent side="bottom" className="text-xs max-w-48">
              {item.label}
            </TooltipContent>
          </Tooltip>
        ))}
      </div>
    </TooltipProvider>
  );
}

function AppTableRow({
  app,
  onStart,
  onStop,
  onRestart,
  onUninstall,
  onOpenLogs,
  onOpenTerminal,
  pendingActions,
  getStatusText,
  getStatusBadgeClass,
  getStatusDotClass,
  isMobile,
}: {
  app: AppTemplate;
  onStart: (appId: string) => void;
  onStop: (appId: string) => void;
  onRestart: (appId: string) => void;
  onUninstall: (appId: string) => void;
  onOpenLogs: (a: AppTemplate) => void;
  onOpenTerminal: (a: AppTemplate) => void;
  pendingActions: Set<string>;
  getStatusText: (status: string | undefined) => string;
  getStatusBadgeClass: (status: string | undefined) => string;
  getStatusDotClass: (status: string | undefined) => string;
  isMobile: boolean;
}) {
  const { t } = useTranslation();
  const [openPopover, setOpenPopover] = useState(false);
  const appId = String(app.id);
  const isRunning = app.state === 'running';

  const shouldFetchStats =    isRunning && !pendingActions.has(`uninstall-${appId}`);
  const { data: statsData } = useAppStats(shouldFetchStats ? appId : '');

  const cpuPercent = statsData?.cpu_usage_percent ?? 0;
  const memoryMB = statsData?.memory_usage_bytes
    ? Math.round((statsData.memory_usage_bytes / 1024 / 1024) * 10) / 10
    : 0;

  return (
    <tr className="group hover:bg-muted/50 transition-colors">
      <td className="px-6 py-4">
        <div className="font-semibold text-foreground">{app.name}</div>
        <div className="text-xs text-muted-foreground mt-1">ID: {appId}</div>
        <InlinePermissionIcons permissions={app.permissions} />
      </td>
      <td className="px-6 py-4">
        <Badge
          variant="outline"
          className={`border-none px-2.5 py-1 rounded-full font-normal ${getStatusBadgeClass(app.state)}`}
        >
          <span
            className={`w-1.5 h-1.5 rounded-full mr-1.5 ${getStatusDotClass(app.state)}`}
          />
          {getStatusText(app.state)}
        </Badge>
      </td>
      <td className="px-6 py-4">
        <div className="flex items-center gap-3">
          <div className="w-24 h-1.5 bg-secondary rounded-full overflow-hidden">
            <div
              className={`h-full rounded-full ${isRunning ? 'bg-primary' : 'bg-muted'}`}
              style={{ width: `${Math.min(cpuPercent, 100)}%` }}
            />
          </div>
          <span className="text-foreground min-w-[50px]">
            {cpuPercent.toFixed(2)}%
          </span>
        </div>
      </td>
      <td className="min-w-[8rem] whitespace-nowrap px-6 py-4 tabular-nums text-foreground">
        {memoryMB > 0 ? `${memoryMB} MB` : '-'}
      </td>
      <td
        className={cn(
          'bg-card px-6 py-4 text-start transition-colors dark:bg-background',
          !isMobile
            && 'sticky right-0 z-9 shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.1)] dark:shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.3)]'
        )}
      >
        <div className="flex items-center gap-1 text-muted-foreground">
          <Button
            variant="ghost"
            size="icon"
            title={t('sys.apps.action.start_stop', '启动/停止')}
            className="h-8 w-8 hover:text-foreground"
            onClick={() => (isRunning ? onStop(appId) : onStart(appId))}
            disabled={
              pendingActions.has(`start-${appId}`)
              || pendingActions.has(`stop-${appId}`)
            }
          >
            {isRunning ? (
              pendingActions.has(`stop-${appId}`) ? (
                <Loader2 className="w-4 h-4 animate-spin" />
              ) : (
                <Square className="w-4 h-4" />
              )
            ) : pendingActions.has(`start-${appId}`) ? (
              <Loader2 className="w-4 h-4 animate-spin" />
            ) : (
              <Play className="w-4 h-4 text-emerald-500" />
            )}
          </Button>
          <Button
            variant="ghost"
            size="icon"
            title={t('sys.apps.action.restart', '重启')}
            className="h-8 w-8 hover:text-foreground disabled:opacity-40"
            onClick={() => onRestart(appId)}
            disabled={!isRunning || pendingActions.has(`restart-${appId}`)}
          >
            {pendingActions.has(`restart-${appId}`) ? (
              <Loader2 className="w-4 h-4 animate-spin" />
            ) : (
              <RotateCw className="w-4 h-4" />
            )}
          </Button>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8 hover:text-foreground disabled:opacity-40"
            title={t('sys.apps.action.console', '控制台')}
            onClick={() => onOpenTerminal(app)}
            disabled={!isRunning}
          >
            <Terminal className="w-4 h-4" />
          </Button>
          <Button
            variant="ghost"
            size="icon"
            className="h-8 w-8 hover:text-foreground"
            title={t('sys.apps.action.logs', '日志')}
            onClick={() => onOpenLogs(app)}
          >
            <FileText className="w-4 h-4" />
          </Button>
          <Popover open={openPopover} onOpenChange={setOpenPopover}>
            <PopoverTrigger asChild>
              <Button
                variant="ghost"
                size="icon"
                title={t('sys.apps.action.uninstall', '卸载')}
                className="h-8 w-8 hover:text-red-600 dark:hover:text-red-500"
                disabled={pendingActions.has(`uninstall-${appId}`)}
              >
                <Trash2 className="w-4 h-4" />
              </Button>
            </PopoverTrigger>
            <PopoverContent className="w-56 p-4" align="end">
              <div className="flex flex-col gap-3">
                <p className="text-sm text-muted-foreground">
                  {t(
                    'sys.apps.message.confirm_uninstall',
                    '确定要卸载此应用吗？'
                  )}
                </p>
                <div className="flex justify-end gap-2">
                  <Button
                    variant="outline"
                    size="sm"
                    className="h-8 px-3 text-xs"
                    onClick={e => {
                      e.stopPropagation();
                      setOpenPopover(false);
                    }}
                  >
                    {t('common.cancel')}
                  </Button>
                  <Button
                    variant="destructive"
                    size="sm"
                    className="h-8 px-3 text-xs"
                    onClick={() => {
                      onUninstall(appId);
                      setOpenPopover(false);
                    }}
                  >
                    {t('common.confirm')}
                  </Button>
                </div>
              </div>
            </PopoverContent>
          </Popover>
        </div>
      </td>
    </tr>
  );
}

interface AppListTableProps {
  apps: AppTemplate[];
  onStart: (appId: string) => void;
  onStop: (appId: string) => void;
  onRestart: (appId: string) => void;
  onUninstall: (appId: string) => void;
  onOpenLogs: (a: AppTemplate) => void;
  onOpenTerminal: (a: AppTemplate) => void;
  pendingActions: Set<string>;
}

const PAGE_SIZE = 10;

export default function AppListTable({
  apps,
  onStart,
  onStop,
  onRestart,
  onUninstall,
  onOpenLogs,
  onOpenTerminal,
  pendingActions,
}: AppListTableProps) {
  const { t } = useTranslation();
  const isMobile = useIsMobile();
  const [currentPage, setCurrentPage] = useState(1);

  const totalPages = Math.max(1, Math.ceil(apps.length / PAGE_SIZE));
  const safePage = Math.min(currentPage, totalPages);
  const startIndex = (safePage - 1) * PAGE_SIZE;
  const pagedApps = apps.slice(startIndex, startIndex + PAGE_SIZE);

  const getStatusText = (status: string | undefined) => {
    switch (status) {
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
  };

  const getStatusBadgeClass = (status: string | undefined) => {
    switch (status) {
      case 'running':
        return 'bg-emerald-500/10 text-emerald-600 dark:text-emerald-500';
      case 'stopped':
        return 'bg-orange-500/10 text-orange-600 dark:text-orange-500';
      case 'failed':
        return 'bg-destructive/10 text-destructive';
      default:
        return 'bg-secondary text-muted-foreground';
    }
  };

  const getStatusDotClass = (status: string | undefined) => {
    switch (status) {
      case 'running':
        return 'bg-emerald-500';
      case 'stopped':
        return 'bg-orange-500';
      case 'failed':
        return 'bg-destructive';
      default:
        return 'bg-muted-foreground';
    }
  };

  return (
    <div className="bg-card rounded-xl border border-border shadow-sm overflow-hidden">
      <ScrollArea className="w-full" type="always">
        <table className="w-full min-w-[700px] text-sm text-left">
          <thead className="border-b border-border bg-background text-muted-foreground font-medium shadow-sm">
            <tr>
              <th className="px-6 py-4 font-medium">
                {t('sys.apps.table.name')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.apps.table.status')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.apps.table.cpu_usage')}
              </th>
              <th className="min-w-[8rem] whitespace-nowrap px-6 py-4 font-medium">
                {t('sys.apps.table.memory_usage')}
              </th>
              <th
                className={cn(
                  'bg-background px-6 py-4 font-medium',
                  !isMobile
                    && 'sticky right-0 top-0 z-10 shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.1)] dark:shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.3)]'
                )}
              >
                {t('sys.apps.table.actions')}
              </th>
            </tr>
          </thead>
          <tbody className="divide-y divide-border">
            {pagedApps.length === 0 ? (
              <tr>
                <td
                  colSpan={5}
                  className="px-6 py-12 text-center text-muted-foreground"
                >
                  <Empty
                    description={t(
                      'sys.apps.empty.installed',
                      '暂无已安装的应用'
                    )}
                  />
                </td>
              </tr>
            ) : (
              pagedApps.map(app => (
                <AppTableRow
                  key={app.id}
                  app={app}
                  onStart={onStart}
                  onStop={onStop}
                  onRestart={onRestart}
                  onUninstall={onUninstall}
                  onOpenLogs={onOpenLogs}
                  onOpenTerminal={onOpenTerminal}
                  pendingActions={pendingActions}
                  getStatusText={getStatusText}
                  getStatusBadgeClass={getStatusBadgeClass}
                  getStatusDotClass={getStatusDotClass}
                  isMobile={isMobile}
                />
              ))
            )}
          </tbody>
        </table>
        <ScrollBar orientation="horizontal" />
      </ScrollArea>

      <div className="px-6 py-4 border-t border-border flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between text-sm text-muted-foreground bg-background/80 dark:bg-muted/20">
        <span>
          {t(
            'sys.apps.pagination.showing',
            'Showing {{start}} to {{end}} of {{total}} containers',
            {
              start: apps.length === 0 ? 0 : startIndex + 1,
              end: Math.min(startIndex + PAGE_SIZE, apps.length),
              total: apps.length,
            }
          )}
        </span>
        <div className="flex gap-1">
          <Button
            variant="outline"
            size="icon"
            className="w-8 h-8 rounded-md disabled:opacity-50"
            disabled={safePage <= 1}
            onClick={() => setCurrentPage(prev => Math.max(1, prev - 1))}
          >
            <ChevronLeft className="w-4 h-4" />
          </Button>
          <Button
            variant="default"
            size="icon"
            className="w-8 h-8 rounded-md bg-primary text-primary-foreground hover:bg-primary/90"
          >
            {safePage}
          </Button>
          <Button
            variant="outline"
            size="icon"
            className="w-8 h-8 rounded-md disabled:opacity-50"
            disabled={safePage >= totalPages}
            onClick={() => setCurrentPage(prev => Math.min(totalPages, prev + 1))}
          >
            <ChevronRight className="w-4 h-4" />
          </Button>
        </div>
      </div>
    </div>
  );
}
