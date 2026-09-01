import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import {
  Download,
  Play,
  Square,
  RotateCw,
  Trash2,
  ExternalLink,
  Video,
  Brain,
  Radio,
  Lightbulb,
  Wifi,
} from 'lucide-react';
import type { AppTemplate, AppPermissions } from '@/services/types';
import {
  useInstallApp,
  useStartApp,
  useStopApp,
  useRestartApp,
  useAppStats,
  useAppPermissions,
} from '@/hooks';
import { getAppWebUrl } from '../lib/appWebUrl';

function PermissionsPanel({ permissions }: { permissions: AppPermissions }) {
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
  const hasNetwork = permissions.network && permissions.network.mode;

  if (!hasVideo && !hasModels && !hasEvents && !hasDevice && !hasNetwork) return null;

  const formatVideoStreamLabel = (stream: string) => {
    switch (stream) {
      case 'main.raw':
        return t('sys.media_settings.main_stream', 'Main Stream');
      case 'sub.raw':
        return t('sys.media_settings.sub_stream', 'Sub Stream');
      case 'third.raw':
        return t('sys.media_settings.third_stream', 'Third Stream');
      default:
        return stream;
    }
  };

  const videoLabels = (permissions.video ?? []).map(formatVideoStreamLabel);

  const deviceLabels: string[] = [];
  if (permissions.device?.light) deviceLabels.push(t('sys.apps.perm.light', 'Light'));
  if (permissions.device?.ir_cut) deviceLabels.push('IR-Cut');
  if (permissions.device?.ptz) deviceLabels.push('PTZ');
  if (permissions.device?.lens) deviceLabels.push(t('sys.apps.perm.lens', 'Lens'));

  return (
    <div className="space-y-3 mb-6">
      <h3 className="text-sm font-semibold text-muted-foreground uppercase">
        {t('sys.apps.detail.permissions', 'Permissions & Resources')}
      </h3>
      <div className="grid grid-cols-1 gap-2.5">
        {hasVideo && (
          <div className="flex items-start gap-3 p-3 rounded-lg bg-muted/40">
            <Video className="w-4 h-4 mt-0.5 text-blue-500 shrink-0" />
            <div className="min-w-0">
              <p className="text-sm font-medium text-foreground">
                {t('sys.apps.perm.video', 'Video Streams')}
              </p>
              <p className="text-xs text-muted-foreground mt-0.5 break-all">
                {videoLabels.join(', ')}
              </p>
            </div>
          </div>
        )}
        {hasModels && (
          <div className="flex items-start gap-3 p-3 rounded-lg bg-muted/40">
            <Brain className="w-4 h-4 mt-0.5 text-purple-500 shrink-0" />
            <div className="min-w-0">
              <p className="text-sm font-medium text-foreground">
                {t('sys.apps.perm.models', 'AI Models')}
              </p>
              <p className="text-xs text-muted-foreground mt-0.5 break-all">
                {permissions.inference!.models!.join(', ')}
                {permissions.inference!.max_qps
                  ? ` (QPS: ${permissions.inference!.max_qps})`
                  : ''}
              </p>
            </div>
          </div>
        )}
        {hasEvents && (
          <div className="flex items-start gap-3 p-3 rounded-lg bg-muted/40">
            <Radio className="w-4 h-4 mt-0.5 text-amber-500 shrink-0" />
            <div className="min-w-0">
              <p className="text-sm font-medium text-foreground">
                {t('sys.apps.perm.events', 'Event Bus')}
              </p>
              <div className="text-xs text-muted-foreground mt-0.5">
                {(permissions.events!.publish?.length ?? 0) > 0 && (
                  <span className="block break-all">
                    {t('sys.apps.perm.pub', 'Publish')}:{' '}
                    {permissions.events!.publish!.join(', ')}
                  </span>
                )}
                {(permissions.events!.subscribe?.length ?? 0) > 0 && (
                  <span className="block break-all">
                    {t('sys.apps.perm.sub', 'Subscribe')}:{' '}
                    {permissions.events!.subscribe!.join(', ')}
                  </span>
                )}
              </div>
            </div>
          </div>
        )}
        {hasDevice && (
          <div className="flex items-start gap-3 p-3 rounded-lg bg-muted/40">
            <Lightbulb className="w-4 h-4 mt-0.5 text-orange-500 shrink-0" />
            <div className="min-w-0">
              <p className="text-sm font-medium text-foreground">
                {t('sys.apps.perm.device', 'Device Control')}
              </p>
              <p className="text-xs text-muted-foreground mt-0.5">
                {deviceLabels.join(', ')}
              </p>
            </div>
          </div>
        )}
        {hasNetwork && (
          <div className="flex items-start gap-3 p-3 rounded-lg bg-muted/40">
            <Wifi className="w-4 h-4 mt-0.5 text-teal-500 shrink-0" />
            <div className="min-w-0">
              <p className="text-sm font-medium text-foreground">
                {t('sys.apps.perm.network', 'Network')}
              </p>
              <p className="text-xs text-muted-foreground mt-0.5">
                {permissions.network!.mode}
                {permissions.network!.inbound
                && permissions.network!.inbound.length > 0
                  ? ` (ports: ${permissions.network!.inbound.join(', ')})`
                  : ''}
              </p>
            </div>
          </div>
        )}
      </div>
    </div>
  );
}

interface AppDetailDialogProps {
  app: AppTemplate | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  onRequestUninstall?: (app: AppTemplate) => void;
  isUninstalling?: boolean;
}

export default function AppDetailDialog({
  app,
  open,
  onOpenChange,
  onRequestUninstall,
  isUninstalling = false,
}: AppDetailDialogProps) {
  const { t, i18n } = useTranslation();
  const installMutation = useInstallApp();
  const startMutation = useStartApp();
  const stopMutation = useStopApp();
  const restartMutation = useRestartApp();

  const isZh = i18n.language === 'zh' || i18n.language === 'zh-CN';
  const isInstalled = app?.state !== undefined;
  const isRunning = app?.state === 'running';

  const { data: statsData } = useAppStats(
    isRunning && app?.id ? String(app.id) : null
  );
  const { data: permissionsData } = useAppPermissions(
    isInstalled && app?.id ? String(app.id) : null
  );
  const permissions: AppPermissions | null | undefined =    permissionsData || app?.permissions;

  if (!app) return null;

  const handleInstall = () => {
    installMutation.mutate(String(app.id));
  };

  const handleStart = () => {
    startMutation.mutate(String(app.id));
  };

  const handleStop = () => {
    stopMutation.mutate(String(app.id));
  };

  const handleRestart = () => {
    restartMutation.mutate(String(app.id));
  };

  const handleUninstall = () => {
    onRequestUninstall?.(app);
  };

  const displayName = isZh ? app.name_zh || app.name : app.name;
  const displayDesc = isZh
    ? app.short_desc_zh || app.short_desc
    : app.short_desc;

  // Format timestamp
  const formatTime = (timestamp: number | undefined) => {
    if (!timestamp) return '-';
    return new Date(timestamp * 1000).toLocaleString(isZh ? 'zh-CN' : 'en-US');
  };

  // Resource usage
  const cpuPercent = statsData?.cpu_usage_percent ?? 0;
  const memoryMB = statsData?.memory_usage_bytes
    ? Math.round((statsData.memory_usage_bytes / 1024 / 1024) * 10) / 10
    : 0;
  const uptimeSeconds = statsData?.uptime_seconds || 0;
  const uptimeHours = Math.floor(uptimeSeconds / 3600);
  const uptimeMins = Math.floor((uptimeSeconds % 3600) / 60);

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-2xl max-h-[90vh] flex flex-col overflow-hidden p-0">
        <DialogHeader className="px-6 pt-6 pb-4 shrink-0">
          <DialogTitle className="sr-only">{displayName}</DialogTitle>
        </DialogHeader>
        <div className="flex-1 min-h-0 overflow-y-auto px-6 pb-6">
          {/* Header */}
          <div className="flex items-start justify-between mb-6">
            <div className="flex gap-4">
              <div className="w-16 h-16 rounded-xl flex items-center justify-center bg-primary/10 text-primary shrink-0">
                <span className="text-2xl font-bold">
                  {displayName.charAt(0)}
                </span>
              </div>

              <div className="flex-1">
                {displayDesc && (
                  <p className="text-muted-foreground text-sm">{displayDesc}</p>
                )}
              </div>
            </div>
          </div>

          {/* Stats for running apps */}
          {isRunning && (
            <div className="grid grid-cols-3 gap-4 mb-6 p-4 bg-muted/50 rounded-lg">
              <div>
                <p className="text-xs text-muted-foreground mb-1">
                  {t('sys.apps.stats.cpu', 'CPU')}
                </p>
                <p className="text-lg font-semibold text-foreground">
                  {cpuPercent.toFixed(2)}%
                </p>
              </div>
              <div>
                <p className="text-xs text-muted-foreground mb-1">
                  {t('sys.apps.stats.memory', 'Memory')}
                </p>
                <p className="text-lg font-semibold text-foreground">
                  {memoryMB} MB
                </p>
              </div>
              <div>
                <p className="text-xs text-muted-foreground mb-1">
                  {t('sys.apps.stats.uptime', 'Uptime')}
                </p>
                <p className="text-lg font-semibold text-foreground">
                  {uptimeHours}h {uptimeMins}m
                </p>
              </div>
            </div>
          )}

          {/* App Info */}
          <div className="space-y-4 mb-6">
            <h3 className="text-sm font-semibold text-muted-foreground uppercase">
              {t('sys.apps.detail.info', 'App Info')}
            </h3>
            <div className="grid grid-cols-2 gap-4 text-sm">
              <div>
                <span className="text-muted-foreground">
                  {t('sys.apps.detail.id', 'ID')}:
                </span>
                <span className="ml-2 text-foreground font-mono">{app.id}</span>
              </div>
              <div>
                <span className="text-muted-foreground">
                  {t('sys.apps.detail.version', 'Version')}:
                </span>
                <span className="ml-2 text-foreground">
                  {app.version || '-'}
                </span>
              </div>
              {isInstalled && (app.installed_at ?? 0) > 0 && (
                <div>
                  <span className="text-muted-foreground">
                    {t('sys.apps.detail.installed_at', 'Installed At')}:
                  </span>
                  <span className="ml-2 text-foreground">
                    {formatTime(app.installed_at)}
                  </span>
                </div>
              )}
              {isInstalled && (app.started_at ?? 0) > 0 && (
                <div>
                  <span className="text-muted-foreground">
                    {t('sys.apps.detail.started_at', 'Started At')}:
                  </span>
                  <span className="ml-2 text-foreground">
                    {formatTime(app.started_at)}
                  </span>
                </div>
              )}
            </div>
          </div>

          {/* Permissions */}
          {isInstalled && permissions && (
            <PermissionsPanel permissions={permissions} />
          )}

          {/* Actions */}
          <div className="flex items-center gap-2 pt-4 border-t">
            {!isInstalled ? (
              <Button
                className="bg-primary hover:bg-primary/90 text-primary-foreground"
                onClick={handleInstall}
                disabled={installMutation.isPending}
              >
                <Download className="h-4 w-4" />
                {installMutation.isPending
                  ? t('sys.apps.action.installing', 'Installing...')
                  : t('sys.apps.action.install', 'Install')}
              </Button>
            ) : (
              <>
                <Button
                  variant="outline"
                  onClick={isRunning ? handleStop : handleStart}
                  disabled={startMutation.isPending || stopMutation.isPending}
                >
                  {isRunning ? (
                    <>
                      <Square className="h-4 w-4" />
                      {t('sys.apps.action.stop', '停止')}
                    </>
                  ) : (
                    <>
                      <Play className="h-4 w-4" />
                      {t('sys.apps.action.start', '启动')}
                    </>
                  )}
                </Button>
                {isRunning && (
                  <Button
                    variant="outline"
                    onClick={handleRestart}
                    disabled={restartMutation.isPending}
                  >
                    <RotateCw className="h-4 w-4" />
                    {t('sys.apps.action.restart', '重启')}
                  </Button>
                )}
                {isRunning && getAppWebUrl(app) && (
                  <Button
                    variant="outline"
                    onClick={() => window.open(getAppWebUrl(app)!, '_blank')}
                  >
                    <ExternalLink className="h-4 w-4" />
                    {t('sys.apps.action.visit', '访问应用')}
                  </Button>
                )}
                <Button
                  variant="destructive"
                  onClick={handleUninstall}
                  disabled={isUninstalling}
                  className="ml-auto"
                >
                  <Trash2 className="h-4 w-4" />
                  {t('sys.apps.action.uninstall', '卸载')}
                </Button>
              </>
            )}
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
