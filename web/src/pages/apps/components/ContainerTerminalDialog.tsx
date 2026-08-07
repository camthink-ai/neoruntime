import { useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { Terminal } from 'xterm';
import { FitAddon } from 'xterm-addon-fit';
import { WebLinksAddon } from 'xterm-addon-web-links';
import 'xterm/css/xterm.css';
import { getItem } from '@/utils/storage';
import { RotateCw } from 'lucide-react';
import { useTheme } from 'next-themes';
import {
  getTerminalTheme,
  TERMINAL_BG_CLASS,
} from '@/pages/maintenance/terminal/terminal-theme';
import { cn } from '@/lib/utils';

interface ContainerTerminalDialogProps {
  containerId: string | null;
  containerName: string;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

export default function ContainerTerminalDialog({
  containerId,
  containerName,
  open,
  onOpenChange,
}: ContainerTerminalDialogProps) {
  const { t } = useTranslation();
  const { resolvedTheme } = useTheme();
  const terminalRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<Terminal | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const [connectionStatus, setConnectionStatus] = useState<
    'connecting' | 'connected' | 'disconnected' | 'error'
  >('disconnected');

  const connect = () => {
    if (!terminalRef.current || !containerId) return;

    // Clean up existing
    if (wsRef.current) {
      wsRef.current.close();
      wsRef.current = null;
    }
    if (xtermRef.current) {
      xtermRef.current.dispose();
      xtermRef.current = null;
    }

    const term = new Terminal({
      cursorBlink: true,
      fontSize: 14,
      fontFamily: 'Consolas, Monaco, "Courier New", monospace',
      theme: getTerminalTheme(resolvedTheme),
    });

    const fitAddon = new FitAddon();
    const webLinksAddon = new WebLinksAddon();
    term.loadAddon(fitAddon);
    term.loadAddon(webLinksAddon);
    term.open(terminalRef.current);

    xtermRef.current = term;
    fitAddonRef.current = fitAddon;

    // Defer fit() until the browser has completed layout so xterm's internal
    // _renderService is fully initialized and .dimensions is available.
    requestAnimationFrame(() => {
      requestAnimationFrame(() => {
        try {
          fitAddon.fit();
        } catch {
          /* ignore if still unmeasurable */
        }
      });
    });

    const { cols, rows } = term;

    // Get token
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }

    const { protocol, host } = window.location;
    const wsProtocol = protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${wsProtocol}//${host}/api/v1/containers/${containerId}/exec/ws?command=/bin/sh&cols=${cols}&rows=${rows}&token=${encodeURIComponent(token)}`;

    setConnectionStatus('connecting');

    const ws = new WebSocket(wsUrl);
    ws.binaryType = 'arraybuffer';
    wsRef.current = ws;

    ws.onopen = () => {
      setConnectionStatus('connected');
      term.clear();
      term.focus();
    };

    ws.onmessage = event => {
      if (event.data instanceof ArrayBuffer) {
        term.write(new Uint8Array(event.data));
      } else {
        term.write(event.data);
      }
    };

    ws.onerror = () => {
      setConnectionStatus('error');
      term.writeln('\r\n\x1b[31mConnection error\x1b[0m');
    };

    ws.onclose = event => {
      setConnectionStatus('disconnected');
      if (event.code !== 1000) {
        term.writeln(`\r\n\x1b[33mConnection closed (${event.code})\x1b[0m`);
      }
    };

    term.onData(data => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(new TextEncoder().encode(data));
      }
    });

    term.onResize(({ cols: newCols, rows: newRows }) => {
      if (ws.readyState === WebSocket.OPEN) {
        ws.send(
          JSON.stringify({
            type: 'resize',
            resize: true,
            cols: newCols,
            rows: newRows,
          })
        );
      }
    });
  };

  useEffect(() => {
    if (open && containerId) {
      // Small delay to let dialog render
      const timer = setTimeout(() => connect(), 100);
      return () => clearTimeout(timer);
    }

    // Cleanup on close
    return () => {
      if (wsRef.current) {
        wsRef.current.close();
        wsRef.current = null;
      }
      if (xtermRef.current) {
        xtermRef.current.dispose();
        xtermRef.current = null;
      }
      fitAddonRef.current = null;
      setConnectionStatus('disconnected');
    };
  }, [open, containerId]);

  useEffect(() => {
    const term = xtermRef.current;
    if (!term) return;
    term.options.theme = getTerminalTheme(resolvedTheme);
  }, [resolvedTheme]);

  // Resize handling
  useEffect(() => {
    if (!open || !terminalRef.current) return;

    const handleWindowResize = () => fitAddonRef.current?.fit();
    window.addEventListener('resize', handleWindowResize);

    const ro = new ResizeObserver(() => fitAddonRef.current?.fit());
    ro.observe(terminalRef.current);

    return () => {
      window.removeEventListener('resize', handleWindowResize);
      ro.disconnect();
    };
  }, [open]);

  // Focus terminal when dialog opens
  useEffect(() => {
    if (open && fitAddonRef.current && xtermRef.current) {
      const timer = setTimeout(() => {
        fitAddonRef.current?.fit();
        xtermRef.current?.focus();
      }, 150);
      return () => clearTimeout(timer);
    }
  }, [open]);

  const getStatusBadge = () => {
    switch (connectionStatus) {
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
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent
        className="max-w-[90vw] sm:max-w-[90vw] w-[1400px] h-[85vh] flex flex-col p-0"
        onOpenAutoFocus={e => e.preventDefault()}
        onCloseAutoFocus={e => e.preventDefault()}
      >
        <DialogHeader className="px-6 pt-6 pb-4 pr-14 border-b shrink-0">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-3">
              <DialogTitle className="text-base">
                Terminal - {containerName}
              </DialogTitle>
              {getStatusBadge()}
            </div>
            {(connectionStatus === 'disconnected'
              || connectionStatus === 'error') && (
              <Button variant="outline" size="sm" onClick={connect}>
                <RotateCw className="w-4 h-4 mr-2" />
                {t('sys.terminal.reconnect', 'Reconnect')}
              </Button>
            )}
          </div>
        </DialogHeader>

        <div
          className={cn(
            'terminal-shell flex-1 min-h-0 mx-6 my-4 rounded-lg font-mono text-sm border border-border overflow-hidden',
            TERMINAL_BG_CLASS
          )}
        >
          <div
            ref={terminalRef}
            className="box-border min-h-0 h-full w-full [&_.xterm]:h-full [&_.xterm-viewport]:overflow-y-auto [&_.xterm-viewport]:overflow-x-hidden [&_.xterm-screen]:overflow-x-hidden"
            onClickCapture={() => xtermRef.current?.focus()}
          />
        </div>
      </DialogContent>
    </Dialog>
  );
}
