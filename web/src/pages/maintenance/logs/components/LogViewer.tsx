import { Button } from '@/components/ui/button';
import { Download, Loader2 } from 'lucide-react';
import { useEffect, useCallback, useRef } from 'react';
import { toast } from 'sonner';
import { useTranslation } from 'react-i18next';
import { logsApi } from '@/services/api/logs';
import type { LogType } from '@/types/log';
import XTermLogViewer from './XTermLogViewer';
import { useLogStream } from '../hooks/useLogStream';

interface LogViewerProps {
  type: LogType;
  target: string | null;
  isLoading: boolean;
}

export default function LogViewer({ type, target, isLoading }: LogViewerProps) {
  const { t } = useTranslation();
  const appendLineCallbackRef = useRef<((line: string) => void) | null>(null);

  // Handle new line from stream
  const handleNewLine = useCallback((line: string) => {
    if (appendLineCallbackRef.current) {
      appendLineCallbackRef.current(line);
    }
  }, []);

  // Set append callback
  const handleAppendLineCallback = useCallback(
    (callback: (line: string) => void) => {
      appendLineCallbackRef.current = callback;
    },
    []
  );

  // WebSocket log stream - always enabled when target is selected
  const { isConnected, error: streamError } = useLogStream({
    type,
    target: target || '',
    enabled: !!target,
    onNewLine: handleNewLine,
  });

  // Show stream error
  useEffect(() => {
    if (streamError) {
      toast.error(streamError);
    }
  }, [streamError]);

  const handleDownload = async () => {
    if (!target) return;
    try {
      const blob = await logsApi.download(type, target);
      const url = window.URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = `${target.split('/').pop() || target}.log`;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      window.URL.revokeObjectURL(url);
      toast.success(t('sys.maintenance.log_viewer.download_success'));
    } catch {
      toast.error(t('sys.maintenance.log_viewer.download_failed'));
    }
  };

  return (
    <div className="flex-1 flex flex-col h-full bg-background overflow-hidden relative">
      {/* Top action bar */}
      <div className="p-4 flex items-center justify-between bg-background">
        <div className="flex items-center gap-3">
          {target && (
            <div className="text-sm">
              <span className="text-muted-foreground">
                {type === 'service'
                  ? `${t('sys.maintenance.log_viewer.service')}: `
                  : `${t('sys.maintenance.log_viewer.file')}: `}
              </span>
              <span className="font-medium text-foreground">
                {type === 'service' ? target : target.split('/').pop()}
              </span>
            </div>
          )}
        </div>
        <div className="flex items-center gap-3">
          <Button
            variant="outline"
            size="sm"
            onClick={handleDownload}
            disabled={!target}
          >
            <Download className="w-4 h-4 mr-2" />
            {t('sys.maintenance.log_viewer.download')}
          </Button>
        </div>
      </div>

      {/* Log terminal view */}
      <div className="flex-1 overflow-hidden relative p-4">
        <div className="w-full h-full bg-[#0d0d0d] rounded-xl overflow-hidden flex flex-col shadow-sm border border-border/50">
          {/* Terminal header */}
          <div className="h-9 bg-[#24130c] flex items-center px-4 text-xs text-gray-400 border-b border-gray-800 shrink-0 justify-between">
            <div className="flex items-center gap-2">
              {isConnected && (
                <span className="px-2 py-0.5 rounded-full bg-green-600/20 text-green-400 text-xs font-medium">
                  LIVE
                </span>
              )}
            </div>
          </div>

          {/* Terminal content */}
          <div className="flex-1 overflow-hidden relative p-2.5">
            {isLoading ? (
              <div className="flex items-center justify-center h-full text-muted-foreground">
                <Loader2 className="w-5 h-5 animate-spin mr-2" />
                <span className="text-sm">
                  {t('sys.maintenance.log_viewer.loading')}
                </span>
              </div>
            ) : !target ? (
              <div className="flex items-center justify-center h-full text-muted-foreground">
                <span className="text-sm">
                  {t('sys.maintenance.log_viewer.select_source')}
                </span>
              </div>
            ) : (
              <XTermLogViewer
                lines={[]}
                isLoading={false}
                streamMode
                onAppendLine={handleAppendLineCallback}
              />
            )}
          </div>
        </div>
      </div>
    </div>
  );
}
