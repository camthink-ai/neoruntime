import { useState, useMemo, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { LayoutGrid, List, Plus, Search } from 'lucide-react';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import { toast } from 'sonner';
import { Skeleton } from '@/components/ui/skeleton';
import {
  useApps,
  useStartApp,
  useStopApp,
  useRestartApp,
  useUninstallApp,
} from '@/hooks';
import AppCardGrid from './components/AppCardGrid';
import AppListTable from './components/AppListTable';
import ImportAppDialog from './components/ImportAppDialog';
import AppDetailDialog from './components/AppDetailDialog';
import ContainerLogsDialog from './components/ContainerLogsDialog';
import ContainerTerminalDialog from './components/ContainerTerminalDialog';
import { MarketplaceSkeleton } from './components/MarketplaceSkeleton';
import type { AppTemplate } from '@/services/types';

export default function Apps() {
  const { t } = useTranslation();

  const [viewMode, setViewMode] = useState<'card' | 'list'>('card');
  const [search, setSearch] = useState('');
  const [statusFilter, setStatusFilter] = useState<
    'all' | 'installed' | 'running' | 'stopped' | 'failed'
  >('all');
  const [importDialogOpen, setImportDialogOpen] = useState(false);

  const [selectedApp, setSelectedApp] = useState<AppTemplate | null>(null);
  const [detailDialogOpen, setDetailDialogOpen] = useState(false);

  const [logsDialogOpen, setLogsDialogOpen] = useState(false);
  const [terminalDialogOpen, setTerminalDialogOpen] = useState(false);
  const [selectedContainer, setSelectedContainer] = useState<{
    id: string;
    name: string;
  } | null>(null);

  const [appToUninstall, setAppToUninstall] = useState<AppTemplate | null>(
    null
  );
  const [showUninstallDialog, setShowUninstallDialog] = useState(false);

  const [pendingActions, setPendingActions] = useState<Set<string>>(new Set());

  const { data: appsData = [], isLoading } = useApps({ refetchInterval: 5000 });

  const startMutation = useStartApp();
  const stopMutation = useStopApp();
  const restartMutation = useRestartApp();
  const uninstallMutation = useUninstallApp();

  const sortedApps: AppTemplate[] = useMemo(() => {
    const apps = Array.isArray(appsData) ? appsData : [];
    return apps
      .map((app: any) => ({
        id: app.id,
        key: app.id,
        name: app.name,
        name_zh: app.name_zh || app.name,
        category: app.category || 'Other',
        version: app.version || '1.0.0',
        author: app.author || 'Unknown',
        short_desc: app.short_desc,
        short_desc_zh: app.short_desc_zh,
        description: app.description,
        state: app.state,
        installed_at: app.installed_at,
        started_at: app.started_at,
        stopped_at: app.stopped_at,
        permissions: app.permissions,
        manifest_path: app.manifest_path,
        image_path: app.image_path,
        isInstalled: app.state !== undefined,
        tags: app.tags || [],
        min_memory: app.min_memory,
        web_url: app.web_url,
        container_id: app.container_id,
      }))
      .sort((a: any, b: any) => {
        const aTime = a.installed_at ? new Date(a.installed_at).getTime() : 0;
        const bTime = b.installed_at ? new Date(b.installed_at).getTime() : 0;
        return bTime - aTime;
      });
  }, [appsData]);

  const filteredApps = useMemo(() => {
    const keyword = search.trim().toLowerCase();
    return sortedApps.filter(app => {
      const matchSearch =        !keyword
        || app.name.toLowerCase().includes(keyword)
        || String(app.id).toLowerCase().includes(keyword);
      const matchStatus = statusFilter === 'all' || app.state === statusFilter;
      return matchSearch && matchStatus;
    });
  }, [sortedApps, search, statusFilter]);

  // Sync selectedApp when detail dialog is open and data changes
  useEffect(() => {
    if (!detailDialogOpen || !selectedApp) return;
    const next = sortedApps.find(a => a.id === selectedApp.id);
    if (!next) return;
    setSelectedApp(prev => {
      if (!prev || prev.id !== next.id) return prev;
      if (
        prev.state === next.state
        && prev.version === next.version
        && prev.installed_at === next.installed_at
        && prev.started_at === next.started_at
        && prev.stopped_at === next.stopped_at
      ) return prev;
      return next;
    });
  }, [detailDialogOpen, selectedApp?.id, sortedApps]);

  // Action handlers
  const handleStart = (appOrId: AppTemplate | string) => {
    const appId = typeof appOrId === 'string' ? appOrId : String(appOrId.id);
    setPendingActions(prev => new Set(prev).add(`start-${appId}`));
    startMutation.mutate(appId, {
      onSuccess: () => {
        toast.success(t('sys.apps.message.start_success', '应用启动成功'));
      },
      onError: () => {
        toast.error(t('sys.apps.message.start_failed', '应用启动失败'));
      },
      onSettled: () => {
        setPendingActions(prev => {
          const n = new Set(prev);
          n.delete(`start-${appId}`);
          return n;
        });
      },
    });
  };

  const handleStop = (appOrId: AppTemplate | string) => {
    const appId = typeof appOrId === 'string' ? appOrId : String(appOrId.id);
    setPendingActions(prev => new Set(prev).add(`stop-${appId}`));
    stopMutation.mutate(appId, {
      onSuccess: () => {
        toast.success(t('sys.apps.message.stop_success', '应用停止成功'));
      },
      onError: () => {
        toast.error(t('sys.apps.message.stop_failed', '应用停止失败'));
      },
      onSettled: () => {
        setPendingActions(prev => {
          const n = new Set(prev);
          n.delete(`stop-${appId}`);
          return n;
        });
      },
    });
  };

  const handleRestart = (appOrId: AppTemplate | string) => {
    const appId = typeof appOrId === 'string' ? appOrId : String(appOrId.id);
    setPendingActions(prev => new Set(prev).add(`restart-${appId}`));
    restartMutation.mutate(appId, {
      onSuccess: () => {
        toast.success(t('sys.apps.message.restart_success', '应用重启成功'));
      },
      onError: () => {
        toast.error(t('sys.apps.message.restart_failed', '应用重启失败'));
      },
      onSettled: () => {
        setPendingActions(prev => {
          const n = new Set(prev);
          n.delete(`restart-${appId}`);
          return n;
        });
      },
    });
  };

  const handleRequestUninstall = (app: AppTemplate) => {
    setAppToUninstall(app);
    setDetailDialogOpen(false);
    window.setTimeout(() => setShowUninstallDialog(true), 0);
  };

  const confirmUninstall = () => {
    if (!appToUninstall) return;
    const appId = String(appToUninstall.id);
    setPendingActions(prev => new Set(prev).add(`uninstall-${appId}`));
    uninstallMutation.mutate(appId, {
      onSuccess: () => {
        toast.success(t('sys.apps.message.uninstall_success', '应用卸载成功'));
        setDetailDialogOpen(false);
      },
      onError: () => {
        toast.error(t('sys.apps.message.uninstall_failed', '应用卸载失败'));
      },
      onSettled: () => {
        setPendingActions(prev => {
          const n = new Set(prev);
          n.delete(`uninstall-${appId}`);
          return n;
        });
      },
    });
    setShowUninstallDialog(false);
  };

  const handleAppClick = (app: AppTemplate) => {
    setSelectedApp(app);
    setDetailDialogOpen(true);
  };

  const handleDetailDialogClose = (open: boolean) => {
    setDetailDialogOpen(open);
    if (!open) setSelectedApp(null);
  };

  const handleOpenLogs = (app: AppTemplate) => {
    const cid = app.container_id || `aipc-${app.id}`;
    setSelectedContainer({ id: cid, name: app.name });

    setLogsDialogOpen(true);
  };

  const handleOpenTerminal = (app: AppTemplate) => {
    const cid = app.container_id || `aipc-${app.id}`;
    setSelectedContainer({ id: cid, name: app.name });

    setTerminalDialogOpen(true);
  };

  if (isLoading) {
    return (
      <div className="p-4 md:p-6 mx-auto max-w-[1600px] min-h-full overflow-auto">
        <div className="flex items-center justify-between mb-6">
          <div className="flex items-center gap-4">
            <div className="flex items-center border rounded-lg p-1">
              <Button variant="secondary" size="sm" className="px-3">
                <LayoutGrid className="w-4 h-4" />
              </Button>
              <Button variant="ghost" size="sm" className="px-3">
                <List className="w-4 h-4" />
              </Button>
            </div>
            <div className="relative flex-1 max-w-md">
              <Skeleton className="h-10 w-full rounded-md" />
            </div>
          </div>
        </div>
        {viewMode === 'card' ? (
          <MarketplaceSkeleton />
        ) : (
          <div className="bg-card rounded-xl border border-border shadow-sm overflow-hidden">
            <table className="w-full text-sm text-left">
              <thead className="border-b border-border bg-background text-muted-foreground font-medium">
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
                  <th className="px-6 py-4 font-medium">
                    {t('sys.apps.table.actions')}
                  </th>
                </tr>
              </thead>
              <tbody>
                <tr>
                  <td colSpan={5} className="py-12 text-center">
                    <div className="animate-pulse text-muted-foreground">
                      Loading...
                    </div>
                  </td>
                </tr>
              </tbody>
            </table>
          </div>
        )}
      </div>
    );
  }

  return (
    <div className="p-4 md:p-6 mx-auto max-w-[1600px] min-h-full overflow-auto">
      {/* Toolbar: column + wrap on small screens, row from md */}
      <div className="mb-6 flex w-full flex-col gap-3 md:flex-row md:flex-wrap md:items-center md:gap-4">
        {/* Mobile: toggle + search one row; md: unwrap via contents */}
        <div className="flex w-full min-w-0 items-center gap-2 md:contents">
          <div className="flex shrink-0 items-center border rounded-lg p-1">
            <Button
              variant={viewMode === 'card' ? 'secondary' : 'ghost'}
              size="sm"
              onClick={() => setViewMode('card')}
              className="px-3"
            >
              <LayoutGrid className="w-4 h-4" />
            </Button>
            <Button
              variant={viewMode === 'list' ? 'secondary' : 'ghost'}
              size="sm"
              onClick={() => setViewMode('list')}
              className="px-3"
            >
              <List className="w-4 h-4" />
            </Button>
          </div>

          <div className="relative min-w-0 flex-1 md:max-w-md md:flex-1">
            <Search className="absolute left-3 top-1/2 -translate-y-1/2 h-4 w-4 text-muted-foreground" />
            <Input
              placeholder={t(
                'sys.apps.action.search_placeholder',
                '搜索应用名称或 ID...'
              )}
              className="pl-9"
              value={search}
              onChange={e => setSearch(e.target.value)}
            />
          </div>
        </div>

        <div className="flex flex-wrap items-center gap-2">
          {(['all', 'installed', 'running', 'stopped', 'failed'] as const).map(
            status => (
              <Button
                key={status}
                variant={statusFilter === status ? 'default' : 'outline'}
                className={`rounded-full px-4 h-9 ${
                  statusFilter === status
                    ? status === 'running'
                      ? 'bg-emerald-500/10 text-emerald-600 dark:text-emerald-500 border-emerald-500/20 hover:bg-emerald-500/20'
                      : status === 'failed'
                        ? 'bg-destructive/10 text-destructive border-destructive/20 hover:bg-destructive/20'
                        : status === 'stopped'
                          ? 'bg-secondary text-foreground border-border hover:bg-secondary/80'
                          : 'bg-card text-foreground border-border shadow-sm hover:bg-accent'
                    : 'bg-transparent border-border text-muted-foreground'
                }`}
                onClick={() => setStatusFilter(status)}
              >
                {status === 'running' && (
                  <span className="w-2 h-2 rounded-full bg-emerald-500 mr-2" />
                )}
                {t(`sys.apps.status.${status === 'all' ? 'all' : status}`)}
              </Button>
            )
          )}
        </div>

        {viewMode === 'list' && (
          <Button
            type="button"
            variant="carbon"
            className="w-auto shrink-0 self-end md:self-auto md:ml-auto"
            onClick={() => setImportDialogOpen(true)}
          >
            <Plus className="w-4 h-4 mr-2" />
            {t('sys.apps.action.import', '导入应用')}
          </Button>
        )}
      </div>

      {/* Content */}
      {viewMode === 'card' ? (
        <AppCardGrid
          apps={filteredApps}
          onAppClick={handleAppClick}
          onStart={handleStart}
          onStop={handleStop}
          onRestart={handleRestart}
          onUninstall={handleRequestUninstall}
          onOpenLogs={handleOpenLogs}
          onOpenTerminal={handleOpenTerminal}
          onImportClick={() => setImportDialogOpen(true)}
          pendingActions={pendingActions}
        />
      ) : (
        <AppListTable
          apps={filteredApps}
          onStart={id => handleStart(id)}
          onStop={id => handleStop(id)}
          onRestart={id => handleRestart(id)}
          onUninstall={id => {
            const app = sortedApps.find(a => String(a.id) === id);
            if (app) handleRequestUninstall(app);
          }}
          onOpenLogs={handleOpenLogs}
          onOpenTerminal={handleOpenTerminal}
          pendingActions={pendingActions}
        />
      )}

      {/* Dialogs */}
      <AppDetailDialog
        app={selectedApp}
        open={detailDialogOpen}
        onOpenChange={handleDetailDialogClose}
        onRequestUninstall={handleRequestUninstall}
        isUninstalling={uninstallMutation.isPending}
      />

      <ImportAppDialog
        open={importDialogOpen}
        onOpenChange={setImportDialogOpen}
      />

      <ContainerLogsDialog
        containerId={selectedContainer?.id || null}
        containerName={selectedContainer?.name || ''}
        open={logsDialogOpen}
        onOpenChange={setLogsDialogOpen}
      />

      <ContainerTerminalDialog
        containerId={selectedContainer?.id || null}
        containerName={selectedContainer?.name || ''}
        open={terminalDialogOpen}
        onOpenChange={setTerminalDialogOpen}
      />

      <AlertDialog
        open={showUninstallDialog}
        onOpenChange={setShowUninstallDialog}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.apps.action.confirm_uninstall', '确认卸载应用')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.apps.action.confirm_uninstall_desc',
                '确定要卸载应用 "{appName}" 吗？此操作无法撤销，应用数据将被永久删除。',
                {
                  appName:
                    appToUninstall?.name_zh || appToUninstall?.name || '',
                }
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={confirmUninstall}
              className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
              disabled={uninstallMutation.isPending}
            >
              {t('sys.apps.action.uninstall', '卸载')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </div>
  );
}
