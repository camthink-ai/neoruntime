import { useTranslation } from 'react-i18next';
import { useQuery } from '@tanstack/react-query';
import {
  Dialog,
  DialogContent,
  DialogFooter,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import { processApi } from '@/services/api/system';
import Loading from '@/components/loading';

interface ProcessDetailDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  pid: number | null;
}

function formatBytes(bytes: number | undefined): string {
  if (!bytes) return '0 B';
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  if (bytes >= 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(2)} MB`;
  if (bytes >= 1024) return `${(bytes / 1024).toFixed(2)} KB`;
  return `${bytes} B`;
}

export function ProcessDetailDialog({
  open,
  onOpenChange,
  pid,
}: ProcessDetailDialogProps) {
  const { t } = useTranslation();

  const { data: processDetail } = useQuery({
    queryKey: ['process', pid],
    queryFn: async () => {
      if (!pid) return null;
      const response = await processApi.getProcessInfo(pid);
      return response.data;
    },
    enabled: open && !!pid,
  });

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="w-[90vw] max-w-lg max-h-[80vh] flex flex-col overflow-hidden">
        <DialogHeader className="shrink-0">
          <DialogTitle>
            {t('maintenance.processes.process_details', 'Process Details')}
          </DialogTitle>
        </DialogHeader>
        <div className="flex-1 min-h-0 overflow-y-auto">
          {processDetail ? (
            <div className="space-y-4">
              <div className="grid grid-cols-2 gap-4">
                <div className="space-y-2">
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_pid', 'Process ID')}
                    </span>
                    <span className="font-mono text-base font-medium">
                      {processDetail.pid}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_name', 'Process Name')}
                    </span>
                    <span className="font-mono text-base font-medium">
                      {processDetail.name}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_username', 'User')}
                    </span>
                    <span className="font-mono text-base">
                      {processDetail.username}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_ppid', 'Parent PID')}
                    </span>
                    <span className="font-mono text-base">
                      {processDetail.ppid}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_status', 'Status')}
                    </span>
                    <span className="font-mono text-base">
                      {Array.isArray(processDetail.status)
                        ? processDetail.status.join(', ')
                        : processDetail.status}
                    </span>
                  </div>
                </div>
                <div className="space-y-2">
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t(
                        'maintenance.processes.detail_cpu_percent',
                        'CPU Usage'
                      )}
                    </span>
                    <span className="font-mono text-base font-medium">
                      {processDetail.cpu_percent?.toFixed(2)}%
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t(
                        'maintenance.processes.detail_mem_percent',
                        'Memory Usage'
                      )}
                    </span>
                    <span className="font-mono text-base font-medium">
                      {processDetail.mem_percent?.toFixed(2)}%
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_mem_rss', 'RSS Memory')}
                    </span>
                    <span className="font-mono text-base">
                      {formatBytes(processDetail.mem_rss)}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_mem_vms', 'VMS Memory')}
                    </span>
                    <span className="font-mono text-base">
                      {formatBytes(processDetail.mem_vms)}
                    </span>
                  </div>
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t('maintenance.processes.detail_num_threads', 'Threads')}
                    </span>
                    <span className="font-mono text-base">
                      {processDetail.num_threads}
                    </span>
                  </div>
                </div>
              </div>

              <div className="space-y-2 border-t pt-4">
                <div className="flex flex-col gap-1">
                  <span className="text-xs text-muted-foreground">
                    {t('maintenance.processes.detail_exe', 'Executable')}
                  </span>
                  <span className="break-all font-mono text-sm">
                    {processDetail.exe}
                  </span>
                </div>
                <div className="flex flex-col gap-1">
                  <span className="text-xs text-muted-foreground">
                    {t('maintenance.processes.detail_cwd', 'Working Directory')}
                  </span>
                  <span className="break-all font-mono text-sm">
                    {processDetail.cwd}
                  </span>
                </div>
                <div className="flex flex-col gap-1">
                  <span className="text-xs text-muted-foreground">
                    {t('maintenance.processes.detail_cmdline', 'Command Line')}
                  </span>
                  <span className="break-all font-mono text-sm">
                    {processDetail.cmdline}
                  </span>
                </div>
                {processDetail.create_time && (
                  <div className="flex flex-col gap-1">
                    <span className="text-sm text-muted-foreground">
                      {t(
                        'maintenance.processes.detail_create_time',
                        'Create Time'
                      )}
                    </span>
                    <span className="font-mono text-sm">
                      {new Date(processDetail.create_time).toLocaleString()}
                    </span>
                  </div>
                )}
              </div>
            </div>
          ) : (
            <Loading fullHeight={false} className="h-32" />
          )}
        </div>
        <DialogFooter className="shrink-0">
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            {t('common.close', 'Close')}
          </Button>
        </DialogFooter>
      </DialogContent>
    </Dialog>
  );
}
