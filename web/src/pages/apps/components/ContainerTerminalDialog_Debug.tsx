import { useEffect, useRef, useState, useCallback } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { ScrollArea } from '@/components/ui/scroll-area';
import { Terminal } from 'xterm';
import { FitAddon } from 'xterm-addon-fit';
import { WebLinksAddon } from 'xterm-addon-web-links';
import 'xterm/css/xterm.css';
import { getItem } from '@/utils/storage';

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
  const terminalRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<Terminal | null>(null);
  const wsRef = useRef<WebSocket | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const [connectionStatus, setConnectionStatus] = useState<
    'connecting' | 'connected' | 'disconnected' | 'error'
  >('disconnected');
  const [debugLog, setDebugLog] = useState<string[]>([]);

  // 使用 useCallback 避免函数在每次渲染时重新创建
  const addDebugLog = useCallback((message: string) => {
    const timestamp = new Date().toLocaleTimeString();
    const logMessage = `[${timestamp}] ${message}`;
    console.log(logMessage);
    setDebugLog(prev => [...prev.slice(-10), logMessage]);
  }, []); // 空依赖数组，函数只创建一次

  useEffect(() => {
    if (!open || !containerId || !terminalRef.current) {
      return;
    }

    addDebugLog(`Initializing terminal for container: ${containerId}`);

    // 创建 xterm 实例
    const term = new Terminal({
      cursorBlink: true,
      fontSize: 14,
      fontFamily: 'Menlo, Monaco, "Courier New", monospace',
      theme: {
        background: '#0f172a',
        foreground: '#e2e8f0',
        cursor: '#22d3ee',
        cursorAccent: '#0f172a',
        selectionBackground: 'rgba(255, 255, 255, 0.3)',
      },
      scrollback: 10000,
      convertEol: true,
    });

    // 添加插件
    const fitAddon = new FitAddon();
    const webLinksAddon = new WebLinksAddon();
    term.loadAddon(fitAddon);
    term.loadAddon(webLinksAddon);

    // 打开终端
    term.open(terminalRef.current);
    setTimeout(() => fitAddon.fit(), 0);

    xtermRef.current = term;
    fitAddonRef.current = fitAddon;

    // 获取 token (stored as "Bearer <secret>")
    let token = getItem<string>('token') || '';
    if (token.startsWith('Bearer ')) {
      token = token.substring(7);
    }
    addDebugLog(
      `Token: ${token ? `${token.substring(0, 10)}...` : 'NOT FOUND'}`
    );

    // 获取终端大小
    const { cols, rows } = term;
    addDebugLog(`Terminal size: ${cols} x ${rows}`);

    // 构建 WebSocket URL
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const { host } = window.location;

    const wsUrl = `${protocol}//${host}/api/v1/containers/${containerId}/exec/ws?token=${encodeURIComponent(token)}`;

    addDebugLog(`WebSocket URL: ${wsUrl}`);
    setConnectionStatus('connecting');

    // 创建 WebSocket 连接
    const ws = new WebSocket(wsUrl);
    wsRef.current = ws;

    ws.onopen = () => {
      addDebugLog('✓ WebSocket connected!');
      setConnectionStatus('connected');

      // 连接成功后，尝试发送终端大小
      try {
        const resizeMsg = JSON.stringify({
          type: 'resize',
          cols,
          rows,
        });
        addDebugLog(`Sending resize: ${resizeMsg}`);
        ws.send(resizeMsg);
      } catch (e) {
        addDebugLog(`Failed to send resize: ${e}`);
      }
    };

    ws.onmessage = event => {
      addDebugLog(
        `← Received data (${typeof event.data}): ${event.data.substring(0, 50)}...`
      );

      if (typeof event.data === 'string') {
        term.write(event.data);
      } else if (event.data instanceof Blob) {
        event.data.text().then(text => {
          addDebugLog(`← Blob decoded: ${text.substring(0, 50)}...`);
          term.write(text);
        });
      } else if (event.data instanceof ArrayBuffer) {
        const text = new TextDecoder().decode(event.data);
        addDebugLog(`← ArrayBuffer decoded: ${text.substring(0, 50)}...`);
        term.write(text);
      }
    };

    ws.onerror = error => {
      addDebugLog(`✗ WebSocket Error: ${error}`);
      setConnectionStatus('error');
      term.writeln('\r\n\x1b[31m✗ WebSocket connection error\x1b[0m\r\n');
    };

    ws.onclose = event => {
      addDebugLog(
        `✗ WebSocket closed: code=${event.code}, reason=${event.reason || 'none'}`
      );
      setConnectionStatus('disconnected');
      if (event.code !== 1000) {
        term.writeln(
          `\r\n\x1b[33m✗ Connection closed: ${event.code}\x1b[0m\r\n`
        );
      }
    };

    // 监听终端输入
    const disposable = term.onData(data => {
      if (ws.readyState === WebSocket.OPEN) {
        // 显示输入的字符码以便调试
        const charCodes = Array.from(data)
          .map(c => c.charCodeAt(0))
          .join(',');
        addDebugLog(`→ Sending: [${charCodes}]`);
        ws.send(data);
      }
    });

    // 清理函数
    return () => {
      disposable.dispose();
      if (wsRef.current) {
        if (wsRef.current.readyState === WebSocket.OPEN) {
          wsRef.current.close(1000, 'Component unmounted');
        }
        wsRef.current = null;
      }
      if (xtermRef.current) {
        xtermRef.current.dispose();
        xtermRef.current = null;
      }
      fitAddonRef.current = null;
      setConnectionStatus('disconnected');
    };
  }, [open, containerId, addDebugLog]); // 添加 addDebugLog 到依赖项

  // 当对话框打开时，调整终端大小
  useEffect(() => {
    if (open && fitAddonRef.current && xtermRef.current) {
      const timer = setTimeout(() => {
        try {
          fitAddonRef.current?.fit();
          xtermRef.current?.focus();
        } catch (error) {
          console.error('Error fitting terminal:', error);
        }
      }, 150);
      return () => clearTimeout(timer);
    }
  }, [open]);

  const getStatusBadge = () => {
    switch (connectionStatus) {
      case 'connecting':
        return (
          <Badge variant="secondary" className="bg-blue-100 text-blue-700">
            <span className="w-2 h-2 bg-blue-500 rounded-full mr-2 animate-pulse" />
            连接中
          </Badge>
        );
      case 'connected':
        return (
          <Badge
            variant="secondary"
            className="bg-emerald-100 text-emerald-700"
          >
            <span className="w-2 h-2 bg-emerald-500 rounded-full mr-2" />
            已连接
          </Badge>
        );
      case 'error':
        return (
          <Badge variant="secondary" className="bg-red-100 text-red-700">
            <span className="w-2 h-2 bg-red-500 rounded-full mr-2" />
            连接失败
          </Badge>
        );
      case 'disconnected':
        return (
          <Badge variant="secondary" className="bg-gray-100 text-gray-700">
            <span className="w-2 h-2 bg-gray-500 rounded-full mr-2" />
            未连接
          </Badge>
        );
      default:
        return null;
    }
  };

  const sendTestCommand = useCallback(() => {
    if (wsRef.current && wsRef.current.readyState === WebSocket.OPEN) {
      const testCmd = 'ls -la\n';
      addDebugLog('Sending test command: ls -la');
      wsRef.current.send(testCmd);
    }
  }, [addDebugLog]);

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="!max-w-[95vw] w-[1600px] h-[90vh] flex flex-col p-0">
        <DialogHeader className="px-6 pt-6 pb-4 pr-14 border-b">
          <div className="flex items-center justify-between">
            <DialogTitle className="flex items-center gap-3">
              <span className="text-xl">💻</span>
              <span>
                {t('sys.apps.terminal.title', '容器终端')} - {containerName}
              </span>
            </DialogTitle>
            <div className="flex items-center gap-2">
              {getStatusBadge()}
              <Button size="sm" variant="outline" onClick={sendTestCommand}>
                发送测试命令
              </Button>
            </div>
          </div>
        </DialogHeader>

        <div className="flex-1 flex gap-4 px-6 py-4 overflow-hidden">
          {/* 终端容器 */}
          <div className="flex-1 overflow-hidden rounded-lg bg-slate-950">
            <div
              ref={terminalRef}
              className="h-full w-full"
              style={{ padding: '12px' }}
            />
          </div>

          {/* 调试日志 */}
          <div className="w-80 flex flex-col">
            <div className="text-sm font-semibold mb-2 text-gray-700">
              调试日志
            </div>
            <ScrollArea className="flex-1 bg-gray-50 rounded-lg p-3 font-mono text-xs">
              <div className="space-y-1 pr-4">
                {debugLog.length === 0 ? (
                  <div className="text-gray-400">等待日志...</div>
                ) : (
                  debugLog.map((log, index) => (
                    <div key={index} className="text-gray-700 break-all">
                      {log}
                    </div>
                  ))
                )}
              </div>
            </ScrollArea>
          </div>
        </div>

        {/* 底部信息栏 */}
        <div className="flex items-center justify-between text-sm text-slate-500 px-6 pb-6 pt-2 border-t">
          <div className="text-xs">调试模式：查看右侧日志了解连接详情</div>
          <div className="text-xs text-slate-400">
            Ctrl+C 中断 | Ctrl+D 退出 | Ctrl+L 清屏
          </div>
        </div>
      </DialogContent>
    </Dialog>
  );
}
