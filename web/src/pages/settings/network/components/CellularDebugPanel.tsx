import { useCallback, useEffect, useState } from 'react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  clearCellularQLog,
  downloadCellularQLog,
  fetchCellularQLogStatus,
  sendCellularAT,
  sendCellularPing,
  startCellularQLog,
  stopCellularQLog,
  type CellularQLogStatus,
} from '@/services/settings';
import {
  Activity,
  ChevronDown,
  ChevronRight,
  Download,
  Loader2,
  Square,
  Terminal,
  Trash2,
} from 'lucide-react';
import { toast } from 'sonner';
import { localizeBackendMessage } from './backendMessages';

type Props = {
  iface?: string;
  disabled?: boolean;
};

function formatBytes(n: number): string {
  if (n < 1024) return `${n} B`;
  if (n < 1024 * 1024) return `${(n / 1024).toFixed(1)} KB`;
  return `${(n / (1024 * 1024)).toFixed(1)} MB`;
}

export default function CellularDebugPanel({ iface = '', disabled }: Props) {
  const { t } = useTranslation();
  const [open, setOpen] = useState(false);

  const [pingHost, setPingHost] = useState('8.8.8.8');
  const [pingBusy, setPingBusy] = useState(false);
  const [pingOut, setPingOut] = useState('');

  const [atCmd, setAtCmd] = useState('AT+CGDCONT?');
  const [atBusy, setAtBusy] = useState(false);
  const [atOut, setAtOut] = useState('');

  const [qlog, setQlog] = useState<CellularQLogStatus | null>(null);
  const [qlogAction, setQlogAction] = useState<
    'start' | 'stop' | 'export' | 'clear' | null
  >(null);

  const refreshQLog = useCallback(async () => {
    try {
      const st = await fetchCellularQLogStatus();
      setQlog(st);
      return st;
    } catch {
      return null;
    }
  }, []);

  useEffect(() => {
    if (!open) return undefined;
    const tick = () => {
      refreshQLog().catch(() => undefined);
    };
    tick();
    const ms = qlogAction === 'stop' ? 500 : 2000;
    const id = window.setInterval(tick, ms);
    return () => window.clearInterval(id);
  }, [open, refreshQLog, qlogAction]);

  const runPing = async () => {
    setPingBusy(true);
    setPingOut('');
    try {
      const res = await sendCellularPing({
        host: pingHost.trim() || '8.8.8.8',
        count: 4,
        iface: iface.trim() || undefined,
      });
      setPingOut(res.output || localizeBackendMessage(res.message, t) || '');
      if (!res.ok) {
        toast.error(localizeBackendMessage(res.message, t) || t('common.error'));
      }
    } catch (err) {
      const msg =        localizeBackendMessage(err instanceof Error ? err.message : null, t)
        || t('common.error');
      setPingOut(msg);
      toast.error(msg);
    } finally {
      setPingBusy(false);
    }
  };

  const runAT = async () => {
    setAtBusy(true);
    setAtOut('');
    try {
      const res = await sendCellularAT(atCmd.trim());
      setAtOut(res.response || localizeBackendMessage(res.message, t) || '');
      if (!res.ok) {
        toast.error(localizeBackendMessage(res.message, t) || t('common.error'));
      }
    } catch (err) {
      const msg =        localizeBackendMessage(err instanceof Error ? err.message : null, t)
        || t('common.error');
      setAtOut(msg);
      toast.error(msg);
    } finally {
      setAtBusy(false);
    }
  };

  const runQLogStart = async () => {
    setQlogAction('start');
    try {
      const res = await startCellularQLog();
      if (!res.ok) {
        toast.error(localizeBackendMessage(res.message, t) || t('common.error'));
        await refreshQLog();
        return;
      }
      setQlog(prev => (prev
          ? {
              ...prev,
              running: true,
              port: res.port || prev.port || prev.dm_port,
              message: res.message || prev.message,
            }
          : prev));
      toast.success(
        t('sys.cellular.debug_qlog_started', 'QLog capture started')
      );
      await refreshQLog();
    } catch (err) {
      toast.error(
        localizeBackendMessage(err instanceof Error ? err.message : null, t)
          || t('common.error')
      );
    } finally {
      setQlogAction(null);
    }
  };

  const runQLogStop = async () => {
    setQlogAction('stop');
    try {
      const res = await stopCellularQLog();
      if (!res.ok) {
        toast.error(
          localizeBackendMessage(res.message, t)
            || t(
              'sys.cellular.debug_qlog_stop_stuck',
              'QLog did not exit; retry or toggle cellular power'
            )
        );
        await refreshQLog();
        return;
      }
      const st = await new Promise<CellularQLogStatus | null>(resolve => {
        let n = 0;
        const step = () => {
          refreshQLog()
            .then(cur => {
              if (!cur?.running || n >= 20) {
                resolve(cur);
                return;
              }
              n += 1;
              window.setTimeout(step, 400);
            })
            .catch(() => resolve(null));
        };
        step();
      });
      if (st && !st.running) {
        toast.success(
          t('sys.cellular.debug_qlog_stopped', 'QLog capture stopped')
        );
      } else if (st?.running) {
        toast.error(
          t(
            'sys.cellular.debug_qlog_stop_stuck',
            'QLog did not exit; retry or toggle cellular power'
          )
        );
      }
    } catch (err) {
      toast.error(
        localizeBackendMessage(err instanceof Error ? err.message : null, t)
          || t('common.error')
      );
    } finally {
      setQlogAction(null);
    }
  };

  const runQLogExport = async () => {
    setQlogAction('export');
    try {
      await downloadCellularQLog();
      toast.success(t('sys.cellular.debug_qlog_exported', 'QLog exported'));
    } catch (err) {
      toast.error(
        localizeBackendMessage(err instanceof Error ? err.message : null, t)
          || t('common.error')
      );
    } finally {
      setQlogAction(null);
    }
  };

  const runQLogClear = async () => {
    setQlogAction('clear');
    try {
      const res = await clearCellularQLog();
      if (!res.ok) {
        toast.error(localizeBackendMessage(res.message, t) || t('common.error'));
      } else {
        toast.success(
          t('sys.cellular.debug_qlog_cleared', 'QLog files cleared')
        );
      }
      await refreshQLog();
    } catch (err) {
      toast.error(
        localizeBackendMessage(err instanceof Error ? err.message : null, t)
          || t('common.error')
      );
    } finally {
      setQlogAction(null);
    }
  };

  const qlogRunning = Boolean(qlog?.running);
  const qlogAvailable = Boolean(qlog?.available);
  const qlogHasFiles = Boolean(qlog && qlog.files > 0);
  const qlogBusy = qlogAction != null;

  return (
    <div className="rounded-xl border border-border p-4">
      <button
        type="button"
        onClick={() => setOpen(v => !v)}
        aria-expanded={open}
        className="flex w-full items-center gap-1.5 text-left text-sm font-semibold text-foreground transition-colors hover:text-foreground/80"
      >
        {open ? (
          <ChevronDown className="size-4 shrink-0 text-muted-foreground" />
        ) : (
          <ChevronRight className="size-4 shrink-0 text-muted-foreground" />
        )}
        {t('sys.cellular.debug_title', 'Advanced / Debug')}
      </button>

      {open ? (
        <div className="mt-4 flex flex-col gap-6">
          <div className="space-y-2">
            <Label>{t('sys.cellular.debug_ping', 'Ping test')}</Label>
            <div className="flex gap-2">
              <Input
                className="min-w-0 flex-1"
                value={pingHost}
                disabled={disabled || pingBusy || !iface}
                onChange={e => setPingHost(e.target.value)}
                placeholder="8.8.8.8"
                onKeyDown={e => {
                  if (e.key === 'Enter' && iface) {
                    runPing().catch(() => undefined);
                  }
                }}
              />
              <Button
                variant="carbon"
                className="shrink-0 gap-2"
                disabled={disabled || pingBusy || !iface}
                onClick={() => {
                  runPing().catch(() => undefined);
                }}
              >
                {pingBusy ? (
                  <Loader2 className="size-4 animate-spin" />
                ) : (
                  <Activity className="size-4" />
                )}
                {t('sys.cellular.debug_ping_run', 'Ping')}
              </Button>
            </div>
            <p className="text-[11px] text-muted-foreground">
              {iface
                ? `${t('sys.cellular.debug_ping_iface', 'Interface')}: ${iface}`
                : t(
                    'sys.cellular.debug_ping_need_session',
                    'Requires an active cellular data session (binds to wwan, not eth0).'
                  )}
            </p>
            {pingOut ? (
              <pre className="max-h-48 overflow-auto rounded-md border border-border bg-muted/40 p-3 font-mono text-[11px] leading-relaxed whitespace-pre-wrap">
                {pingOut}
              </pre>
            ) : null}
          </div>

          <div className="space-y-2">
            <Label>{t('sys.cellular.debug_at', 'AT command')}</Label>
            <div className="flex gap-2">
              <Input
                className="min-w-0 flex-1 font-mono"
                value={atCmd}
                disabled={disabled || atBusy}
                onChange={e => setAtCmd(e.target.value)}
                placeholder="AT+CGDCONT?"
                onKeyDown={e => {
                  if (e.key === 'Enter') {
                    runAT().catch(() => undefined);
                  }
                }}
              />
              <Button
                variant="carbon"
                className="shrink-0 gap-2"
                disabled={disabled || atBusy || !atCmd.trim()}
                onClick={() => {
                  runAT().catch(() => undefined);
                }}
              >
                {atBusy ? (
                  <Loader2 className="size-4 animate-spin" />
                ) : (
                  <Terminal className="size-4" />
                )}
                {t('sys.cellular.debug_at_send', 'Send')}
              </Button>
            </div>
            {atOut ? (
              <pre className="max-h-56 overflow-auto rounded-md border border-border bg-muted/40 p-3 font-mono text-[11px] leading-relaxed whitespace-pre-wrap">
                {atOut}
              </pre>
            ) : null}
          </div>

          <div className="space-y-2">
            <Label>{t('sys.cellular.debug_qlog', 'QLog')}</Label>
            <p className="text-[11px] text-muted-foreground">
              {qlog
                ? qlogRunning
                  ? t('sys.cellular.debug_qlog_status_running', {
                      port: qlog.port || qlog.dm_port || '—',
                      files: qlog.files,
                      size: formatBytes(qlog.bytes),
                      defaultValue:
                        'Capturing on {{port}} · {{files}} files · {{size}}',
                    })
                  : qlogAvailable
                    ? t('sys.cellular.debug_qlog_status_idle', {
                        port: qlog.dm_port || '—',
                        files: qlog.files,
                        size: formatBytes(qlog.bytes),
                        defaultValue:
                          'Idle · DM {{port}} · {{files}} files · {{size}}',
                      })
                    : t(
                        'sys.cellular.debug_qlog_unavailable',
                        'qlog binary not installed on device'
                      )
                : t('common.loading', 'Loading...')}
            </p>
            <div className="flex flex-wrap justify-end gap-2">
              {qlogRunning ? (
                <Button
                  variant="carbon"
                  className="gap-2"
                  disabled={disabled || qlogBusy}
                  onClick={() => {
                    runQLogStop().catch(() => undefined);
                  }}
                >
                  {qlogAction === 'stop' ? (
                    <Loader2 className="size-4 animate-spin" />
                  ) : (
                    <Square className="size-4" />
                  )}
                  {t('sys.cellular.debug_qlog_stop', 'Stop')}
                </Button>
              ) : (
                <Button
                  variant="carbon"
                  className="gap-2"
                  disabled={disabled || qlogBusy || !qlogAvailable}
                  onClick={() => {
                    runQLogStart().catch(() => undefined);
                  }}
                >
                  {qlogAction === 'start' ? (
                    <Loader2 className="size-4 animate-spin" />
                  ) : (
                    <Activity className="size-4" />
                  )}
                  {t('sys.cellular.debug_qlog_start', 'Start')}
                </Button>
              )}
              <Button
                variant="carbon"
                className="gap-2"
                disabled={disabled || qlogBusy || !qlogHasFiles}
                onClick={() => {
                  runQLogExport().catch(() => undefined);
                }}
              >
                {qlogAction === 'export' ? (
                  <Loader2 className="size-4 animate-spin" />
                ) : (
                  <Download className="size-4" />
                )}
                {t('sys.cellular.debug_qlog_export', 'Export')}
              </Button>
              <Button
                variant="carbon"
                className="gap-2"
                disabled={disabled || qlogBusy || qlogRunning || !qlogHasFiles}
                onClick={() => {
                  runQLogClear().catch(() => undefined);
                }}
              >
                {qlogAction === 'clear' ? (
                  <Loader2 className="size-4 animate-spin" />
                ) : (
                  <Trash2 className="size-4" />
                )}
                {t('sys.cellular.debug_qlog_clear', 'Clear')}
              </Button>
            </div>
          </div>
        </div>
      ) : null}
    </div>
  );
}
