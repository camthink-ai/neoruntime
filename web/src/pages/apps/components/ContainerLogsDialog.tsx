import { useState, useEffect, useRef, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { Terminal, Download, Trash2, Pause, Play } from 'lucide-react';
import { getItem } from '@/utils/storage';
import { TERMINAL_BG_CLASS } from '@/pages/maintenance/terminal/terminal-theme';
import { cn } from '@/lib/utils';

interface ContainerLogsDialogProps {
  containerId: string | null;
  containerName: string;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export default function ContainerLogsDialog({
  containerId,
  containerName,
  open,
  onOpenChange,
}: ContainerLogsDialogProps) {
  const { t } = useTranslation();
  const [logs, setLogs] = useState<string[]>([]);
  const [isConnected, setIsConnected] = useState(false);
  const [isPaused, setIsPaused] = useState(false);
  const isPausedRef = useRef(false);
  const scrollRef = useRef<HTMLDivElement>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const pausedLogsRef = useRef<string[]>([]);
  const shouldAutoScroll = useRef(true);
  const isProgrammaticScroll = useRef(false);

  useEffect(() => {
    isPausedRef.current = isPaused;
  }, [isPaused]);

  const handleScroll = useCallback(() => {
    if (isProgrammaticScroll.current) return;
    const el = scrollRef.current;
    if (!el) return;
    const atBottom = el.scrollHeight - el.scrollTop - el.clientHeight < 40;
    shouldAutoScroll.current = atBottom;
  }, []);

  useEffect(() => {
    if (shouldAutoScroll.current && !isPaused) {
      const el = scrollRef.current;
      if (el) {
        isProgrammaticScroll.current = true;
        el.scrollTop = el.scrollHeight;
        requestAnimationFrame(() => {
          isProgrammaticScroll.current = false;
        });
      }
    }
  }, [logs, isPaused]);

  useEffect(() => {
    if (!open || !containerId) {
      return;
    }

    setLogs([]);
    pausedLogsRef.current = [];
    setIsPaused(false);
    isPausedRef.current = false;

    // Get token from storage (stored as "Bearer <secret>")
    let token = getItem<string>('token') || '';
    // Remove "Bearer " prefix for WebSocket query parameter
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const { host } = window.location;
    const wsUrl = `${protocol}//${host}/api/v1/containers/${containerId}/logs/ws?tail=200&token=${encodeURIComponent(token)}`;

    const ws = new WebSocket(wsUrl);
    wsRef.current = ws;

    ws.onopen = () => {
      setIsConnected(true);
    };

    ws.onmessage = event => {
      const logLine = event.data as string;
      if (isPausedRef.current) {
        pausedLogsRef.current.push(logLine);
      } else {
        setLogs(prev => [...prev, logLine]);
      }
    };

    ws.onerror = () => {
      setIsConnected(false);
    };

    ws.onclose = () => {
      setIsConnected(false);
    };

    return () => {
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
      }
      setIsConnected(false);
    };
  }, [open, containerId]);

  const togglePause = useCallback(() => {
    setIsPaused(prev => {
      if (prev) {
        setLogs(prevLogs => [...prevLogs, ...pausedLogsRef.current]);
        pausedLogsRef.current = [];
      }
      return !prev;
    });
  }, []);

  const clearLogs = useCallback(() => {
    setLogs([]);
    pausedLogsRef.current = [];
  }, []);

  const downloadLogs = useCallback(() => {
    const logContent = logs.join('\n');
    const blob = new Blob([logContent], { type: 'text/plain' });
    const url = window.URL.createObjectURL(blob);
    const link = document.createElement('a');
    link.href = url;
    link.download = `${containerName}-logs-${Date.now()}.txt`;
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
    window.URL.revokeObjectURL(url);
  }, [logs, containerName]);

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="max-w-[90vw] sm:max-w-[90vw] w-[1400px] h-[85vh] flex flex-col p-0">
        <DialogHeader className="px-6 pt-6 pb-4 pr-14 border-b">
          <div className="flex items-center justify-between gap-4">
            <div className="flex items-center gap-3 flex-1 min-w-0">
              <Terminal className="w-5 h-5 flex-shrink-0" />
              <DialogTitle className="truncate">
                {containerName} - {t('sys.apps.logs.title')}
              </DialogTitle>
              {isConnected && (
                <Badge
                  variant="secondary"
                  className="bg-emerald-100 text-emerald-700 flex-shrink-0"
                >
                  <span className="w-2 h-2 bg-emerald-500 rounded-full mr-2 animate-pulse" />
                  {t('sys.apps.logs.live_stream')}
                </Badge>
              )}
              {isPaused && (
                <Badge
                  variant="secondary"
                  className="bg-amber-100 text-amber-700 flex-shrink-0"
                >
                  {t('sys.apps.logs.paused')}
                </Badge>
              )}
              {!isConnected && open && (
                <Badge
                  variant="secondary"
                  className="bg-red-100 text-red-700 flex-shrink-0"
                >
                  {t('sys.apps.logs.disconnected')}
                </Badge>
              )}
            </div>
            <div className="flex items-center gap-2 flex-shrink-0">
              <Button
                variant="outline"
                size="sm"
                onClick={togglePause}
                title={
                  isPaused
                    ? t('common.resume', '恢复')
                    : t('common.pause', '暂停')
                }
              >
                {isPaused ? (
                  <Play className="w-4 h-4" />
                ) : (
                  <Pause className="w-4 h-4" />
                )}
              </Button>
              <Button
                variant="outline"
                size="sm"
                onClick={clearLogs}
                title={t('sys.apps.logs.clear')}
              >
                <Trash2 className="w-4 h-4" />
              </Button>
              <Button
                variant="outline"
                size="sm"
                onClick={downloadLogs}
                disabled={logs.length === 0}
                title={t('sys.apps.logs.download')}
              >
                <Download className="w-4 h-4" />
              </Button>
            </div>
          </div>
        </DialogHeader>

        <div
          ref={scrollRef}
          onScroll={handleScroll}
          className={cn(
            'terminal-shell flex-1 min-h-0 mx-6 my-4 rounded-lg font-mono text-sm border border-border overflow-y-auto',
            TERMINAL_BG_CLASS
          )}
        >
          <div className="py-2.5 pl-2.5 pr-4">
            {logs.length === 0 ? (
              <div className="text-slate-500 text-center py-8">
                {isConnected
                  ? t('sys.apps.logs.waiting')
                  : t('sys.apps.logs.no_logs')}
              </div>
            ) : (
              logs.map((log, index) => (
                <div
                  key={index}
                  className="text-slate-200 hover:bg-slate-900 px-2 py-0.5 rounded leading-5"
                >
                  <span className="text-slate-500 mr-3 select-none">
                    {String(index + 1).padStart(4, '0')}
                  </span>
                  <span className="whitespace-pre-wrap break-all">{log}</span>
                </div>
              ))
            )}
          </div>
        </div>

        {/* 底部信息栏 */}
        <div className="flex items-center justify-between text-sm text-slate-500 px-6 pb-6 pt-2 border-t">
          <div>
            {t('sys.apps.logs.total_lines', { count: logs.length })}
            {pausedLogsRef.current.length > 0 && (
              <span className="ml-2 text-amber-600">
                {t('sys.apps.logs.buffered_lines', {
                  count: pausedLogsRef.current.length,
                })}
              </span>
            )}
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
