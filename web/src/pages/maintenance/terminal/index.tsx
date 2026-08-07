import { useEffect, useRef, useState, useCallback } from 'react';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Terminal as XTerm } from 'xterm';
import { FitAddon } from 'xterm-addon-fit';
import { WebLinksAddon } from 'xterm-addon-web-links';
import 'xterm/css/xterm.css';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import { Checkbox } from '@/components/ui/checkbox';
import Loading from '@/components/loading';
import { getItem } from '@/utils/storage';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogTrigger,
} from '@/components/ui/dialog';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Separator } from '@/components/ui/separator';
import { Switch } from '@/components/ui/switch';
import { sshApi } from '@/services/api';
import { toast } from 'sonner';
import { Save, Settings, RotateCw } from 'lucide-react';
import { useTheme } from 'next-themes';
import { cn } from '@/lib/utils';
import {
  getTerminalTheme,
  TERMINAL_BG_CLASS,
  TERMINAL_INNER_PADDING_CLASS,
} from './terminal-theme';
import {
  configureXtermTextarea,
  getDigitFromKeyboardEvent,
  isNumpadDecimalKey,
  isNumpadEnterKey,
  normalizeTerminalInput,
} from './terminal-input';
import {
  formatInputForLog,
  installTerminalDebugBanner,
  logTerminalDebug,
} from './terminal-debug';

// Reconnection config
const RECONNECT_BASE_DELAY = 1000;
const RECONNECT_MAX_DELAY = 10000;
const RECONNECT_MAX_RETRIES = 5;
const RECONNECT_JITTER_MAX = 300;

type ConnectionStatus = 'connecting' | 'connected' | 'disconnected' | 'error';

interface SSHConfigFormData {
  port: string;
  permit_root_login: 'yes' | 'no' | 'prohibit-password';
  password_authentication: 'yes' | 'no';
  pubkey_authentication: 'yes' | 'no';
  max_auth_tries: string;
}

function Terminal() {
  const { t } = useTranslation();
  const { resolvedTheme } = useTheme();
  const terminalRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<XTerm | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const disposedRef = useRef(false);
  const lastInterceptedDigitRef = useRef<{ digit: string; ts: number } | null>(
    null
  );
  const fitRafRef = useRef<number | null>(null);
  const openRafRef = useRef<number | null>(null);
  const [status, setStatus] = useState<ConnectionStatus>('disconnected');
  const [, setRetryCount] = useState(0);
  const [maxRetriesReached, setMaxRetriesReached] = useState(false);
  const [reconnectPaused, setReconnectPaused] = useState(false);
  const shouldReconnectRef = useRef(false);
  const retryTimerRef = useRef<ReturnType<typeof setTimeout> | null>(null);
  const [sshDialogOpen, setSshDialogOpen] = useState(false);
  const [config, setConfig] = useState<SSHConfigFormData>({
    port: '22',
    permit_root_login: 'yes',
    password_authentication: 'yes',
    pubkey_authentication: 'yes',
    max_auth_tries: '6',
  });
  const [restartService, setRestartService] = useState(false);
  const queryClient = useQueryClient();

  // --- WebSocket connection ---
  const getWsUrl = useCallback(() => {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    return `${protocol}//${window.location.host}/api/v1/terminal/ws?token=${encodeURIComponent(token)}`;
  }, []);

  const connect = useCallback(
    (xterm: XTerm) => {
      // Clean up existing connection
      if (wsRef.current) {
        shouldReconnectRef.current = false;
        wsRef.current.close();
        wsRef.current = null;
      }

      setStatus('connecting');
      setMaxRetriesReached(false);
      setReconnectPaused(false);

      const wsUrl = getWsUrl();
      const ws = new WebSocket(wsUrl);
      wsRef.current = ws;
      shouldReconnectRef.current = true;

      ws.onopen = () => {
        setStatus('connected');
        xterm.focus();
        ws.send(
          JSON.stringify({ type: 'resize', cols: xterm.cols, rows: xterm.rows })
        );
      };

      ws.onmessage = event => {
        const raw = event.data;
        logTerminalDebug('recv', {
          raw: typeof raw === 'string' ? raw.slice(0, 200) : raw,
          len: typeof raw === 'string' ? raw.length : undefined,
        });

        // If login attempts are exhausted, backend will close soon. Do NOT reconnect.
        if (
          typeof raw === 'string'
          && (raw.includes('Maximum login attempts exceeded')
            || raw.includes('Login incorrect')
            || raw.includes('Connection closed'))
        ) {
          // Only stop reconnect on the terminal final-state messages. We treat
          // "Maximum login attempts exceeded" as terminal.
          if (
            raw.includes('Maximum login attempts exceeded')
            || raw.includes('Connection closed')
          ) {
            shouldReconnectRef.current = false;
            setReconnectPaused(true);
            try {
              ws.close(1000, 'auth-failed');
            } catch {
              // ignore close errors
            }
          }
        }

        // Backend sends raw PTY/login text. Only unwrap explicit JSON envelopes.
        // NOTE: JSON.parse("6") === 6 — single-digit echoes must NOT go through JSON.parse.
        if (typeof raw === 'string' && raw.startsWith('{')) {
          try {
            const msg = JSON.parse(raw) as { type?: string; data?: string };
            if (msg.type === 'output' && msg.data) {
              xterm.write(msg.data);
              return;
            }
          } catch {
            // not JSON — write as terminal output below
          }
        }

        xterm.write(raw);
      };

      ws.onclose = event => {
        setStatus('disconnected');
        logTerminalDebug('close', {
          code: event.code,
          reason: event.reason,
          wasClean: event.wasClean,
        });
        if (
          shouldReconnectRef.current
          && event.code !== 1000
          && event.code !== 1001
        ) {
          scheduleReconnect(xterm);
        }
      };

      ws.onerror = () => {
        setStatus('error');
      };
    },
    [getWsUrl]
  );

  const scheduleReconnect = useCallback(
    (xterm: XTerm) => {
      if (retryTimerRef.current) {
        clearTimeout(retryTimerRef.current);
        retryTimerRef.current = null;
      }

      setRetryCount(prev => {
        if (prev >= RECONNECT_MAX_RETRIES) {
          setMaxRetriesReached(true);
          setReconnectPaused(true);
          // Stop any further automatic reconnect attempts until user clicks reconnect.
          shouldReconnectRef.current = false;
          return prev;
        }

        const next = prev + 1;
        const delay =          Math.min(RECONNECT_BASE_DELAY * 2 ** prev, RECONNECT_MAX_DELAY)
          + Math.floor(Math.random() * RECONNECT_JITTER_MAX);

        retryTimerRef.current = setTimeout(() => {
          if (shouldReconnectRef.current) {
            connect(xterm);
          }
        }, delay);

        return next;
      });
    },
    [connect]
  );

  const handleManualReconnect = useCallback(() => {
    setRetryCount(0);
    setMaxRetriesReached(false);
    setReconnectPaused(false);
    if (xtermRef.current) {
      xtermRef.current.clear();
      connect(xtermRef.current);
    }
  }, [connect]);

  // --- Initialize terminal ---
  useEffect(() => {
    if (!terminalRef.current) return;

    disposedRef.current = false;

    const xterm = new XTerm({
      cursorBlink: true,
      cursorStyle: 'block',
      fontSize: 14,
      fontFamily: 'Consolas, Monaco, "Courier New", monospace',
      theme: getTerminalTheme(undefined),
    });

    const fitAddon = new FitAddon();
    const webLinksAddon = new WebLinksAddon();

    xterm.loadAddon(fitAddon);
    xterm.loadAddon(webLinksAddon);

    xtermRef.current = xterm;
    fitAddonRef.current = fitAddon;

    const safeFit = () => {
      if (disposedRef.current) return;
      const container = terminalRef.current;
      const fitAddonInstance = fitAddonRef.current;
      const term = xtermRef.current;

      if (!container || !fitAddonInstance || !term) return;
      // When the terminal is hidden/collapsed, FitAddon can crash internally.
      if (container.clientWidth === 0 || container.clientHeight === 0) return;

      if (fitRafRef.current !== null) {
        cancelAnimationFrame(fitRafRef.current);
      }

      fitRafRef.current = requestAnimationFrame(() => {
        fitRafRef.current = null;
        if (disposedRef.current) return;
        try {
          fitAddonInstance.fit();
        } catch {
          return;
        }
        if (wsRef.current?.readyState === WebSocket.OPEN) {
          wsRef.current.send(
            JSON.stringify({ type: 'resize', cols: term.cols, rows: term.rows })
          );
        }
      });
    };

    // xterm.open() may throw when the container is still 0x0 (e.g. during initial
    // layout or React StrictMode effect double-invocation). We wait until the
    // container has measurable dimensions before opening.
    let openAttempts = 0;
    const tryOpenWhenReady = () => {
      if (disposedRef.current) return;
      const container = terminalRef.current;
      if (!container) return;
      if (container.clientWidth === 0 || container.clientHeight === 0) {
        if (openAttempts++ < 20) {
          openRafRef.current = requestAnimationFrame(tryOpenWhenReady);
        }
        return;
      }
      try {
        xterm.open(container);
        configureXtermTextarea(container);
      } catch {
        if (openAttempts++ < 20) {
          openRafRef.current = requestAnimationFrame(tryOpenWhenReady);
        }
        return;
      }

      // Once opened, defer first fit/connect until layout is settled.
      requestAnimationFrame(() => {
        requestAnimationFrame(() => {
          safeFit();
          connect(xterm);
        });
      });
    };

    openRafRef.current = requestAnimationFrame(tryOpenWhenReady);

    window.addEventListener('resize', safeFit);

    const ro = new ResizeObserver(safeFit);
    ro.observe(terminalRef.current);

    installTerminalDebugBanner();

    const sendTerminalInput = (data: string, source: string) => {
      const normalized = normalizeTerminalInput(data);
      const ws = wsRef.current;

      if (!data) return;

      if (ws?.readyState !== WebSocket.OPEN) {
        logTerminalDebug('send-blocked', {
          source,
          reason: 'ws not open',
          readyState: ws?.readyState ?? 'no socket',
          data: formatInputForLog(normalized),
        });
        return;
      }

      const payload = JSON.stringify({ type: 'input', data: normalized });
      logTerminalDebug('send', {
        source,
        data: formatInputForLog(normalized),
        payload,
      });
      ws.send(payload);
    };

    xterm.attachCustomKeyEventHandler(event => {
      if (event.type !== 'keydown' || event.repeat) return true;
      if (event.ctrlKey || event.altKey || event.metaKey) return true;

      const digit = getDigitFromKeyboardEvent(event);
      // Only intercept NUMPAD digits. Main-row digits should flow through xterm's
      // normal input pipeline and be emitted via onData, otherwise we'll double-send.
      const isNumpad =        event.location === KeyboardEvent.DOM_KEY_LOCATION_NUMPAD
        || event.code.startsWith('Numpad');

      if (digit && isNumpad) {
        logTerminalDebug('keydown', {
          key: event.key,
          code: event.code,
          location: event.location,
          digit,
          path: 'customKeyHandler',
        });
        lastInterceptedDigitRef.current = { digit, ts: Date.now() };
        sendTerminalInput(digit, 'keydown-digit-numpad');
        return false;
      }
      if (isNumpadDecimalKey(event)) {
        sendTerminalInput('.', 'keydown-decimal');
        return false;
      }
      if (isNumpadEnterKey(event)) {
        sendTerminalInput('\r', 'keydown-enter');
        return false;
      }

      return true;
    });

    xterm.onData(data => {
      logTerminalDebug('onData', { raw: formatInputForLog(data) });
      // If we just intercepted a numpad digit on keydown, xterm may still emit
      // an onData sequence (e.g. SS3/CSI) representing the same key. Avoid
      // sending both.
      const normalized = normalizeTerminalInput(data);
      const last = lastInterceptedDigitRef.current;
      if (
        last
        && Date.now() - last.ts < 50
        && /^[0-9]$/.test(normalized)
        && normalized === last.digit
      ) {
        logTerminalDebug('dedupe', {
          reason: 'recent numpad intercept',
          onData: formatInputForLog(data),
          normalized: formatInputForLog(normalized),
        });
        return;
      }

      sendTerminalInput(data, 'onData');
    });

    return () => {
      disposedRef.current = true;
      if (openRafRef.current !== null) {
        cancelAnimationFrame(openRafRef.current);
        openRafRef.current = null;
      }
      if (fitRafRef.current !== null) {
        cancelAnimationFrame(fitRafRef.current);
        fitRafRef.current = null;
      }
      window.removeEventListener('resize', safeFit);
      ro.disconnect();
      shouldReconnectRef.current = false;
      if (retryTimerRef.current) {
        clearTimeout(retryTimerRef.current);
      }
      wsRef.current?.close();
      xterm.dispose();
      xtermRef.current = null;
      fitAddonRef.current = null;
    };
  }, []);

  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;
    term.options.theme = getTerminalTheme(resolvedTheme);
  }, [resolvedTheme]);

  // --- SSH config ---
  const { isLoading: isConfigLoading, refetch: refetchConfig } = useQuery({
    queryKey: ['sshConfig'],
    queryFn: async () => {
      const response = await sshApi.getConfig();
      const configData = response.data?.config;

      if (configData) {
        setConfig({
          port: configData.Port || '22',
          permit_root_login: (configData.PermitRootLogin?.toLowerCase()
            || 'yes') as any,
          password_authentication:
            (configData.PasswordAuthentication?.toLowerCase() || 'yes') as any,
          pubkey_authentication:
            (configData.PubkeyAuthentication?.toLowerCase() || 'yes') as any,
          max_auth_tries: configData.MaxAuthTries || '6',
        });
      }

      return configData;
    },
    enabled: sshDialogOpen,
  });

  const { refetch: refetchStatus } = useQuery({
    queryKey: ['sshStatus'],
    queryFn: async () => {
      const response = await sshApi.getStatus();
      return response.data?.status;
    },
    refetchInterval: 5000,
  });

  const updateMutation = useMutation({
    mutationFn: (data: SSHConfigFormData & { restart_service?: boolean }) => sshApi.setConfig(data),
    onSuccess: () => {
      toast.success(t('ssh.update_success', 'SSH 配置已更新'));
      queryClient.invalidateQueries({ queryKey: ['sshConfig'] });
      queryClient.invalidateQueries({ queryKey: ['sshStatus'] });
      if (restartService) {
        setTimeout(() => {
          refetchStatus();
        }, 2000);
      }
    },
    onError: (error: any) => {
      toast.error(
        t('ssh.update_error', 'SSH 配置更新失败：')
          + (error?.response?.data?.message || error?.message || error)
      );
    },
  });

  const parsePositiveInt = (value: string) => {
    const n = Number(value);
    if (!Number.isFinite(n)) return null;
    const i = Math.trunc(n);
    return i > 0 ? i : null;
  };

  const validateAndNormalizeSSHConfig = (value: SSHConfigFormData) => {
    const port = parsePositiveInt(value.port);
    if (!port || port < 1 || port > 65535) {
      return {
        ok: false as const,
        message: t('ssh.port_invalid', '端口范围应为 1-65535'),
      };
    }

    const maxAuthTries = parsePositiveInt(value.max_auth_tries);
    if (!maxAuthTries || maxAuthTries < 1 || maxAuthTries > 10) {
      return {
        ok: false as const,
        message: t(
          'ssh.max_auth_tries_invalid',
          '最大认证尝试次数范围应为 1-10'
        ),
      };
    }

    return {
      ok: true as const,
      value: {
        ...value,
        port: String(port),
        max_auth_tries: String(maxAuthTries),
      },
    };
  };

  const handleDigitOnlyChange = (
    field: 'port' | 'max_auth_tries',
    raw: string,
    maxLen: number
  ) => {
    const next = raw.replace(/\D/g, '').slice(0, maxLen);
    setConfig(prev => ({ ...prev, [field]: next }));
  };

  const handleSave = () => {
    const validated = validateAndNormalizeSSHConfig(config);
    if (!validated.ok) {
      toast.error(validated.message);
      return;
    }
    updateMutation.mutate({
      ...validated.value,
      restart_service: restartService,
    });
  };

  const handleDialogOpenChange = (open: boolean) => {
    setSshDialogOpen(open);
    if (open) {
      refetchConfig();
    }
  };

  // --- Status badge ---
  const getStatusBadge = () => {
    switch (status) {
      case 'connecting':
        return (
          <Badge
            variant="secondary"
            className="bg-blue-100 text-blue-700 dark:bg-blue-900/30 dark:text-blue-400"
          >
            <span className="w-2 h-2 bg-blue-500 rounded-full mr-2 animate-pulse" />
            {t('sys.terminal.status.connecting', 'Connecting')}
          </Badge>
        );
      case 'connected':
        return (
          <Badge
            variant="secondary"
            className="bg-emerald-100 text-emerald-700 dark:bg-emerald-900/30 dark:text-emerald-400"
          >
            <span className="w-2 h-2 bg-emerald-500 rounded-full mr-2" />
            {t('sys.terminal.status.connected', 'Connected')}
          </Badge>
        );
      case 'error':
        return (
          <Badge
            variant="secondary"
            className="bg-red-100 text-red-700 dark:bg-red-900/30 dark:text-red-400"
          >
            <span className="w-2 h-2 bg-red-500 rounded-full mr-2" />
            {t('sys.terminal.status.error', 'Connection Failed')}
          </Badge>
        );
      case 'disconnected':
        return (
          <Badge
            variant="secondary"
            className="bg-gray-100 text-gray-700 dark:bg-gray-800 dark:text-gray-400"
          >
            <span className="w-2 h-2 bg-gray-500 rounded-full mr-2" />
            {t('sys.terminal.status.disconnected', 'Disconnected')}
          </Badge>
        );
      default:
        return null;
    }
  };

  return (
    <div className="flex h-full flex-col overflow-x-hidden p-4 md:p-6">
      <div className="mb-4 flex items-center justify-between">
        <div className="flex items-center gap-2">
          {getStatusBadge()}
          {(maxRetriesReached || reconnectPaused) && status !== 'connected' && (
            <Button
              type="button"
              variant="outline"
              size="sm"
              onClick={handleManualReconnect}
            >
              <RotateCw className="w-4 h-4 mr-2" />
              {t('common.reconnect', 'Reconnect')}
            </Button>
          )}
        </div>
        <Dialog open={sshDialogOpen} onOpenChange={handleDialogOpenChange}>
          <DialogTrigger asChild>
            <Button type="button" variant="outline" size="sm">
              <Settings className="w-4 h-4 mr-2" />
              {t('common.ssh_settings', 'SSH 配置')}
            </Button>
          </DialogTrigger>
          <DialogContent className="max-w-lg">
            <DialogHeader>
              <DialogTitle>{t('common.ssh_settings', 'SSH 配置')}</DialogTitle>
            </DialogHeader>

            {isConfigLoading ? (
              <Loading fullHeight={false} className="h-32" />
            ) : (
              <div className="space-y-4">
                <div className="space-y-2 flex items-center justify-between gap-8">
                  <Label htmlFor="port" className="shrink-0 m-0">
                    {t('ssh.port', 'Port')}
                  </Label>
                  <Input
                    id="port"
                    type="text"
                    inputMode="numeric"
                    autoComplete="off"
                    value={config.port}
                    onChange={e => handleDigitOnlyChange('port', e.target.value, 5)}
                    placeholder="22"
                    className="w-40"
                  />
                </div>

                <div className="space-y-2 flex items-center justify-between gap-8">
                  <Label htmlFor="permit-root">
                    {t('ssh.permit_root_login', 'Root 登录')}
                  </Label>
                  <Select
                    value={config.permit_root_login}
                    onValueChange={(value: any) => setConfig({ ...config, permit_root_login: value })}
                  >
                    <SelectTrigger id="permit-root" className="w-40">
                      <SelectValue />
                    </SelectTrigger>
                    <SelectContent>
                      <SelectItem value="yes">
                        {t('ssh.permit_root.yes', 'Allow')}
                      </SelectItem>
                      <SelectItem value="prohibit-password">
                        {t('ssh.permit_root.prohibit_password', 'Key Only')}
                      </SelectItem>
                      <SelectItem value="no">
                        {t('ssh.permit_root.no', 'Deny')}
                      </SelectItem>
                    </SelectContent>
                  </Select>
                </div>

                <div className="space-y-2 flex items-center justify-between gap-8">
                  <Label htmlFor="password-auth">
                    {t(
                      'ssh.password_authentication',
                      'Password Authentication'
                    )}
                  </Label>
                  <Switch
                    id="password-auth"
                    checked={config.password_authentication === 'yes'}
                    onCheckedChange={checked => setConfig({
                        ...config,
                        password_authentication: checked ? 'yes' : 'no',
                      })}
                  />
                </div>

                <div className="space-y-2 flex items-center justify-between gap-8">
                  <Label htmlFor="pubkey-auth">
                    {t(
                      'ssh.pubkey_authentication',
                      'Public Key Authentication'
                    )}
                  </Label>
                  <Switch
                    id="pubkey-auth"
                    checked={config.pubkey_authentication === 'yes'}
                    onCheckedChange={checked => setConfig({
                        ...config,
                        pubkey_authentication: checked ? 'yes' : 'no',
                      })}
                  />
                </div>

                <div className="space-y-2 flex items-center justify-between gap-8">
                  <Label htmlFor="max-tries" className="shrink-0 m-0">
                    {t('ssh.max_auth_tries', 'Max Auth Attempts')}
                  </Label>
                  <Input
                    id="max-tries"
                    type="text"
                    inputMode="numeric"
                    autoComplete="off"
                    value={config.max_auth_tries}
                    onChange={e => handleDigitOnlyChange('max_auth_tries', e.target.value, 2)}
                    placeholder="6"
                    className="w-40"
                  />
                </div>

                <Separator />

                <div className="flex items-center space-x-2">
                  <Checkbox
                    id="restart-service"
                    checked={restartService}
                    onCheckedChange={checked => setRestartService(!!checked)}
                  />
                  <Label htmlFor="restart-service" className="cursor-pointer">
                    {t(
                      'ssh.restart_service',
                      'Restart SSH service after saving'
                    )}
                  </Label>
                </div>

                <div className="w-full flex justify-end pt-2">
                  <Button
                    variant="carbon"
                    onClick={handleSave}
                    disabled={updateMutation.isPending}
                    className="w-32"
                  >
                    <Save className="w-4 h-4 mr-2" />
                    {updateMutation.isPending
                      ? t('common.saving', 'Saving...')
                      : t('common.save', 'Save Config')}
                  </Button>
                </div>
              </div>
            )}
          </DialogContent>
        </Dialog>
      </div>
      <div
        className={cn(
          'terminal-shell flex h-0 min-h-0 flex-1 flex-col overflow-hidden rounded-2xl border border-border',
          TERMINAL_INNER_PADDING_CLASS,
          TERMINAL_BG_CLASS
        )}
      >
        <div
          ref={terminalRef}
          className="box-border min-h-0 h-full w-full [&_.xterm]:h-full [&_.xterm-viewport]:overflow-y-auto [&_.xterm-viewport]:overflow-x-hidden [&_.xterm-screen]:overflow-x-hidden"
          onClickCapture={() => xtermRef.current?.focus()}
        />
      </div>
    </div>
  );
}

export default Terminal;
