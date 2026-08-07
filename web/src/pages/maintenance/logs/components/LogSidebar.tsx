import { useTranslation } from 'react-i18next';
import { FileText, Server, FileStack } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { ScrollArea } from '@/components/ui/scroll-area';
import { cn } from '@/lib/utils';
import type { LogFile, LogService, LogType } from '@/types/log';

interface LogSidebarProps {
  services: LogService[];
  files: LogFile[];
  viewMode: LogType;
  onViewModeChange: (mode: LogType) => void;
  selectedTarget: string | null;
  onSelectTarget: (target: string) => void;
}

function formatBytes(bytes: number): string {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return `${(bytes / k ** i).toFixed(1)} ${sizes[i]}`;
}

export default function LogSidebar({
  services,
  files,
  viewMode,
  onViewModeChange,
  selectedTarget,
  onSelectTarget,
}: LogSidebarProps) {
  const { t } = useTranslation();

  return (
    <div className="w-72 shrink-0 border-r border-border bg-card overflow-hidden flex flex-col h-full">
      {/* View Mode Tabs */}
      <div className="p-3 border-b border-border">
        <div className="flex bg-muted/30 rounded-md p-1 gap-2">
          <Button
            variant="ghost"
            size="sm"
            onClick={() => onViewModeChange('service')}
            className={cn(
              'flex-1 gap-1.5',
              viewMode === 'service'
                ? 'bg-[#f24a00] text-white shadow-sm'
                : 'text-muted-foreground'
            )}
          >
            <Server className="w-3.5 h-3.5" />
            <span>
              {t('sys.maintenance.log_sidebar.service_log', '服务日志')}
            </span>
          </Button>
          <Button
            variant="ghost"
            size="sm"
            onClick={() => onViewModeChange('file')}
            className={cn(
              'flex-1 gap-1.5',
              viewMode === 'file'
                ? 'bg-[#f24a00] text-white shadow-sm'
                : 'text-muted-foreground'
            )}
          >
            <FileStack className="w-3.5 h-3.5" />
            <span>{t('sys.maintenance.log_sidebar.file_log', '文件日志')}</span>
          </Button>
        </div>
      </div>

      {/* List */}
      <ScrollArea className="flex-1 min-h-0">
        <h3 className="text-xs font-medium text-muted-foreground px-4 py-3 uppercase tracking-wide">
          {viewMode === 'service'
            ? t('sys.maintenance.log_sidebar.service_list', '服务列表')
            : t('sys.maintenance.log_sidebar.log_files', '日志文件')}
        </h3>

        {viewMode === 'service' ? (
          <div className="px-2 pb-4 space-y-1">
            {services.map(service => {
              const isSelected = selectedTarget === service.id;
              return (
                <Button
                  key={service.id}
                  variant="ghost"
                  onClick={() => onSelectTarget(service.id)}
                  className={cn(
                    'w-full justify-start h-auto p-3 gap-3',
                    isSelected
                      ? 'bg-[#fff5f0] dark:bg-orange-950/20 border border-[#f24a00]/30 hover:bg-[#fff5f0]/80 dark:hover:bg-orange-950/30'
                      : 'hover:bg-muted'
                  )}
                >
                  <Server
                    className={cn(
                      'w-4 h-4 shrink-0',
                      isSelected ? 'text-[#f24a00]' : 'text-muted-foreground'
                    )}
                  />
                  <div className="flex-1 overflow-hidden text-left">
                    <div
                      className={cn(
                        'text-sm font-medium truncate',
                        isSelected ? 'text-[#f24a00]' : 'text-foreground'
                      )}
                    >
                      {service.name}
                    </div>
                    <div className="text-xs text-muted-foreground truncate">
                      {service.id}
                    </div>
                  </div>
                </Button>
              );
            })}
            {services.length === 0 && (
              <div className="px-4 py-8 text-center text-sm text-muted-foreground">
                {t('sys.maintenance.log_sidebar.no_services', '没有可用的服务')}
              </div>
            )}
          </div>
        ) : (
          <div className="px-2 pb-4 space-y-1">
            {files.map(file => {
              const isSelected = selectedTarget === file.path;
              return (
                <Button
                  key={file.id}
                  variant="ghost"
                  onClick={() => onSelectTarget(file.path)}
                  className={cn(
                    'w-full justify-start h-auto p-3 gap-3',
                    isSelected
                      ? 'bg-[#fff5f0] dark:bg-orange-950/20 border border-[#f24a00]/30 hover:bg-[#fff5f0]/80 dark:hover:bg-orange-950/30'
                      : 'hover:bg-muted'
                  )}
                >
                  <FileText
                    className={cn(
                      'w-4 h-4 mt-0.5 shrink-0',
                      isSelected ? 'text-[#f24a00]' : 'text-muted-foreground'
                    )}
                  />
                  <div className="flex-1 overflow-hidden text-left">
                    <div
                      className={cn(
                        'text-sm font-medium truncate',
                        isSelected ? 'text-[#f24a00]' : 'text-foreground'
                      )}
                    >
                      {file.name}
                    </div>
                    <div className="flex items-center gap-2 mt-1 text-xs text-muted-foreground">
                      <span>{formatBytes(file.size)}</span>
                      {file.service && (
                        <>
                          <span>·</span>
                          <span className="text-[#f24a00]/70">
                            {file.service}
                          </span>
                        </>
                      )}
                    </div>
                  </div>
                </Button>
              );
            })}
            {files.length === 0 && (
              <div className="px-4 py-8 text-center text-sm text-muted-foreground">
                {t('sys.maintenance.log_sidebar.no_files', '没有找到日志文件')}
              </div>
            )}
          </div>
        )}
      </ScrollArea>
    </div>
  );
}
