import { useTranslation } from 'react-i18next';
import { Card } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import {
  BarChart2,
  Monitor,
  Shield,
  HardDrive,
  Network,
  Play,
  Square,
  RotateCw,
  Loader2,
  Terminal,
  FileText,
  Trash2,
  Activity,
  Video,
  Brain,
  Wifi,
  Lightbulb,
  Radio,
  ExternalLink,
} from 'lucide-react';
import {
  Tooltip,
  TooltipContent,
  TooltipProvider,
  TooltipTrigger,
} from '@/components/ui/tooltip';
import { useAppStats } from '@/hooks';
import type { AppTemplate, AppPermissions } from '@/services/types';
import { Separator } from '@/components/ui/separator';
import { getAppWebUrl } from '../lib/appWebUrl';

interface TemplateCardProps {
  app: AppTemplate;
  onCardClick: (app: AppTemplate) => void;
  onStart: (app: AppTemplate) => void;
  onStop: (app: AppTemplate) => void;
  onRestart: (app: AppTemplate) => void;
  onUninstall: (app: AppTemplate) => void;
  onOpenLogs: (app: AppTemplate) => void;
  onOpenTerminal: (app: AppTemplate) => void;
  pendingActions: Set<string>;
}

const getIcon = (keyName: string, className: string) => {
  switch (keyName) {
    case 'data-analytics':
      return <BarChart2 className={className} />;
    case 'device-mgmt':
      return <Monitor className={className} />;
    case 'security-audit':
      return <Shield className={className} />;
    case 'external-backup':
      return <HardDrive className={className} />;
    case 'api-gateway':
      return <Network className={className} />;
    default:
      return <Monitor className={className} />;
  }
};

function PermissionIcons({ permissions }: { permissions: AppPermissions }) {
  const { t } = useTranslation();
  const hasVideo = permissions.video && permissions.video.length > 0;
  const hasModels =    permissions.inference?.models && permissions.inference.models.length > 0;
  const hasEvents =    permissions.events
    && ((permissions.events.publish?.length ?? 0) > 0
      || (permissions.events.subscribe?.length ?? 0) > 0);
  const hasDevice =    permissions.device
    && (permissions.device.light
      || permissions.device.ir_cut
      || permissions.device.ptz
      || permissions.device.lens);
  const hasNetwork = permissions.network?.mode;

  if (!hasVideo && !hasModels && !hasEvents && !hasDevice && !hasNetwork) return null;

  const items: { icon: React.ReactNode; label: string; color: string }[] = [];
  if (hasVideo) {
    items.push({
      icon: <Video className="w-3.5 h-3.5" />,
      label: permissions.video!.join(', '),
      color: 'text-blue-500',
    });
  }
  if (hasModels) {
    items.push({
      icon: <Brain className="w-3.5 h-3.5" />,
      label: permissions.inference!.models!.join(', '),
      color: 'text-purple-500',
    });
  }
  if (hasEvents) {
    items.push({
      icon: <Radio className="w-3.5 h-3.5" />,
      label: t('sys.apps.permissions.events', 'Events'),
      color: 'text-amber-500',
    });
  }
  if (hasDevice) {
    items.push({
      icon: <Lightbulb className="w-3.5 h-3.5" />,
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
  if (hasNetwork) {
    items.push({
      icon: <Wifi className="w-3.5 h-3.5" />,
      label: permissions.network!.mode!,
      color: 'text-teal-500',
    });
  }

  return (
    <TooltipProvider delayDuration={200}>
      <div className="flex items-center gap-1.5">
        {items.map((item, i) => (
          <Tooltip key={i}>
            <TooltipTrigger asChild>
              <span
                className={`inline-flex items-center justify-center w-6 h-6 rounded-md bg-muted/60 ${item.color} cursor-default`}
              >
                {item.icon}
              </span>
            </TooltipTrigger>
            <TooltipContent side="bottom" className="text-xs">
              {item.label}
            </TooltipContent>
          </Tooltip>
        ))}
      </div>
    </TooltipProvider>
  );
}

export default function TemplateCard({
  app,
  onCardClick,
  onStart,
  onStop,
  onRestart,
  onUninstall,
  onOpenLogs,
  onOpenTerminal,
  pendingActions = new Set(),
}: TemplateCardProps) {
  const { t, i18n } = useTranslation();
  const appId = String(app.id);

  const isRunning = app.state === 'running';
  const isInstalled = app.state !== undefined;
  const shouldFetchStats =    isRunning && !pendingActions.has(`uninstall-${appId}`);
  const { data: statsData } = useAppStats(shouldFetchStats ? appId : '');

  const lang = i18n.language;
  const displayName = lang === 'zh' ? app.name_zh || app.name : app.name;

  const cpuPercent = statsData?.cpu_usage_percent ?? 0;
  const memoryMB = statsData?.memory_usage_bytes
    ? Math.round((statsData.memory_usage_bytes / 1024 / 1024) * 10) / 10
    : 0;

  const getStateText = (state: string | undefined) => {
    switch (state) {
      case 'installed':
        return t('sys.apps.status.installed', 'Installed');
      case 'running':
        return t('sys.apps.status.running', 'Running');
      case 'stopped':
        return t('sys.apps.status.stopped', 'Stopped');
      case 'failed':
        return t('sys.apps.status.failed', 'Failed');
      default:
        return state ? t('sys.apps.status.unknown', 'Unknown') : '';
    }
  };

  const formatRelativeTime = (timestamp: number | undefined) => {
    if (!timestamp || timestamp === 0) return null;
    const now = Date.now() / 1000;
    const diff = now - timestamp;
    if (diff < 60) return t('sys.apps.time.just_now', '刚刚');
    if (diff < 3600) return `${Math.floor(diff / 60)} ${t('sys.apps.time.minutes_ago', '分钟前')}`;
    if (diff < 86400) return `${Math.floor(diff / 3600)} ${t('sys.apps.time.hours_ago', '小时前')}`;
    return `${Math.floor(diff / 86400)} ${t('sys.apps.time.days_ago', '天前')}`;
  };

  const installedTime = formatRelativeTime(app.installed_at);
  const stoppedTime = formatRelativeTime(app.stopped_at);
  const isStopped = app.state === 'stopped';

  return (
    <Card
      className="flex flex-col p-6 justify-between shadow-sm border-border hover:shadow-md transition-shadow cursor-pointer min-h-[220px]"
      onClick={() => onCardClick(app)}
    >
      <div className="flex flex-col gap-4">
        <div className="flex items-start justify-between">
          <div className="w-12 h-12 rounded-xl flex items-center justify-center bg-primary/10 text-primary">
            {getIcon(app.key || 'default', 'w-6 h-6')}
          </div>

          <div className="flex flex-wrap gap-2 justify-end pl-2">
            {isInstalled && (
              <Badge
                variant="outline"
                className={`border-none px-2.5 py-1 rounded-full font-normal ${
                  isRunning
                    ? 'bg-emerald-500/10 text-emerald-600 dark:text-emerald-500'
                    : 'bg-muted text-muted-foreground'
                }`}
              >
                <span
                  className={`w-1.5 h-1.5 rounded-full mr-1.5 ${
                    isRunning
                      ? 'bg-emerald-500 animate-pulse'
                      : 'bg-muted-foreground'
                  }`}
                />
                {getStateText(app.state)}
              </Badge>
            )}
          </div>
        </div>

        <div>
          <h3 className="text-lg font-bold text-foreground leading-tight">
            {displayName}
          </h3>
          <p className="text-sm text-muted-foreground mt-2 leading-relaxed line-clamp-2">
            {lang === 'zh'
              ? app.short_desc_zh || app.short_desc || app.description
              : app.short_desc || app.description}
          </p>
        </div>

        {isInstalled && app.permissions && (
          <PermissionIcons permissions={app.permissions} />
        )}

        <div className="text-xs font-medium text-muted-foreground">
          {t('sys.apps.import.version', '版本')} : {app.version || '-'}
        </div>

        {isRunning && (
          <div className="flex items-center gap-4 text-xs text-muted-foreground">
            <div className="flex items-center gap-1.5">
              <Activity className="w-3.5 h-3.5" />
              <span>
                {t('sys.apps.resource.cpu', 'CPU')}: {cpuPercent.toFixed(2)}%
              </span>
            </div>
            <div className="flex items-center gap-1.5">
              <span>
                {t('sys.apps.resource.memory', '内存')}: {memoryMB}MB
              </span>
            </div>
          </div>
        )}

        {isInstalled && !isRunning && installedTime && (
          <div className="text-xs text-muted-foreground">
            {isStopped && stoppedTime ? (
              <span>
                {t('sys.apps.time.last_run', '上次运行')}: {stoppedTime}
              </span>
            ) : (
              <span>
                {t('sys.apps.time.installed', '安装于')}: {installedTime}
              </span>
            )}
          </div>
        )}
      </div>

      <div>
        <Separator className="mt-4 mb-2" />
        <div className="flex items-center justify-end">
          {!isInstalled ? null : (
            <div className="flex items-center gap-1">
              <Button
                variant="ghost"
                size="icon"
                className="h-8 w-8 hover:text-foreground"
                onClick={e => {
                  e.stopPropagation();
                  if (isRunning) {
                    onStop(app);
                  } else {
                    onStart(app);
                  }
                }}
                disabled={
                  pendingActions.has(`start-${appId}`)
                  || pendingActions.has(`stop-${appId}`)
                }
                title={
                  isRunning
                    ? t('sys.apps.action.stop', '停止')
                    : t('sys.apps.action.start', '启动')
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
              {isRunning && (
                <Button
                  variant="ghost"
                  size="icon"
                  className="h-8 w-8 hover:text-foreground"
                  onClick={e => {
                    e.stopPropagation();
                    onRestart(app);
                  }}
                  disabled={pendingActions.has(`restart-${appId}`)}
                  title={t('sys.apps.action.restart', '重启')}
                >
                  {pendingActions.has(`restart-${appId}`) ? (
                    <Loader2 className="w-4 h-4 animate-spin" />
                  ) : (
                    <RotateCw className="w-4 h-4" />
                  )}
                </Button>
              )}
              {isRunning && (
                <Button
                  variant="ghost"
                  size="icon"
                  className="h-8 w-8 hover:text-foreground"
                  onClick={e => {
                    e.stopPropagation();
                    onOpenLogs(app);
                  }}
                  title={t('sys.apps.action.logs', '日志')}
                >
                  <FileText className="w-4 h-4" />
                </Button>
              )}
              {isRunning && (
                <Button
                  variant="ghost"
                  size="icon"
                  className="h-8 w-8 hover:text-foreground"
                  onClick={e => {
                    e.stopPropagation();
                    onOpenTerminal(app);
                  }}
                  title={t('sys.apps.action.console', '控制台')}
                >
                  <Terminal className="w-4 h-4" />
                </Button>
              )}
              {isRunning && getAppWebUrl(app) && (
                <Button
                  variant="ghost"
                  size="icon"
                  className="h-8 w-8 hover:text-foreground"
                  onClick={e => {
                    e.stopPropagation();
                    window.open(getAppWebUrl(app)!, '_blank');
                  }}
                  title={t('sys.apps.action.visit', '访问应用')}
                >
                  <ExternalLink className="w-4 h-4" />
                </Button>
              )}
              <Button
                variant="ghost"
                size="icon"
                className="h-8 w-8 hover:text-destructive"
                onClick={e => {
                  e.stopPropagation();
                  onUninstall(app);
                }}
                disabled={pendingActions.has(`uninstall-${appId}`)}
                title={t('sys.apps.action.uninstall', '卸载')}
              >
                <Trash2 className="w-4 h-4" />
              </Button>
            </div>
          )}
        </div>
      </div>
    </Card>
  );
}
