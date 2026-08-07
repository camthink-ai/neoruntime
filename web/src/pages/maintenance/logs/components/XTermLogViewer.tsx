import { useEffect, useRef, useCallback } from 'react';
import { Terminal } from 'xterm';
import { FitAddon } from 'xterm-addon-fit';
import { WebLinksAddon } from 'xterm-addon-web-links';
import 'xterm/css/xterm.css';
import type { LogLine } from '@/types/log';

interface XTermLogViewerProps {
  lines: LogLine[];
  isLoading?: boolean;
  streamMode?: boolean;
  onAppendLine?: (callback: (line: string) => void) => void;
}

export default function XTermLogViewer({
  lines,
  isLoading,
  streamMode = false,
  onAppendLine,
}: XTermLogViewerProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const terminalRef = useRef<HTMLDivElement>(null);
  const xtermRef = useRef<Terminal | null>(null);
  const fitAddonRef = useRef<FitAddon | null>(null);
  const isInitializedRef = useRef(false);
  const lineCountRef = useRef(0);
  const streamBufferRef = useRef<string[]>([]);
  const flushRafRef = useRef<number | null>(null);

  // Initialize terminal only once
  useEffect(() => {
    if (!terminalRef.current || isInitializedRef.current) return;

    isInitializedRef.current = true;

    // Initialize xterm
    const term = new Terminal({
      cursorBlink: false,
      fontSize: 13,
      fontFamily: 'Consolas, Monaco, "Courier New", monospace',
      theme: {
        background: '#0d0d0d',
        foreground: '#d4d4d4',
        cursor: '#d4d4d4',
        cursorAccent: '#0d0d0d',
        selectionBackground: '#4fc3f7',
        black: '#000000',
        red: '#cd3131',
        green: '#0dbc79',
        yellow: '#e5e510',
        blue: '#2472c8',
        magenta: '#bc3fbc',
        cyan: '#11a8cd',
        white: '#e5e5e5',
        brightBlack: '#666666',
        brightRed: '#f14c4c',
        brightGreen: '#23d18b',
        brightYellow: '#f5f543',
        brightBlue: '#3b8eea',
        brightMagenta: '#d670d6',
        brightCyan: '#29b8db',
        brightWhite: '#ffffff',
      },
      convertEol: true,
      disableStdin: true,
      cursorStyle: 'bar',
      scrollback: 10000,
    });

    // Add addons
    const fitAddon = new FitAddon();
    const webLinksAddon = new WebLinksAddon();

    term.loadAddon(fitAddon);
    term.loadAddon(webLinksAddon);

    // Open terminal
    term.open(terminalRef.current);

    xtermRef.current = term;
    fitAddonRef.current = fitAddon;

    // Initial fit
    requestAnimationFrame(() => {
      fitAddon.fit();
    });

    // Handle window resize only
    const handleResize = () => {
      requestAnimationFrame(() => {
        fitAddon.fit();
      });
    };

    window.addEventListener('resize', handleResize);

    // Keep xterm fit synced with container size changes.
    // (Used instead of ScrollArea to avoid height chain issues.)
    const ro = new ResizeObserver(() => {
      requestAnimationFrame(() => {
        fitAddon.fit();
      });
    });
    ro.observe(terminalRef.current);

    return () => {
      window.removeEventListener('resize', handleResize);
      ro.disconnect();
      term.dispose();
      isInitializedRef.current = false;
    };
  }, []);

  // Flush buffered stream lines in a single write per animation frame
  const flushStreamBuffer = useCallback(() => {
    flushRafRef.current = null;
    const term = xtermRef.current;
    if (!term || streamBufferRef.current.length === 0) return;

    const chunk = `${streamBufferRef.current.join('\r\n')}\r\n`;
    streamBufferRef.current = [];
    term.write(chunk, () => {
      term.scrollToBottom();
    });
  }, []);

  // Set up append line callback for stream mode
  useEffect(() => {
    if (!streamMode || !onAppendLine || !xtermRef.current) return;

    const appendLine = (rawLine: string) => {
      if (!xtermRef.current) return;

      const parsedLine = parseLogLine(rawLine, ++lineCountRef.current);
      streamBufferRef.current.push(parsedLine);

      if (flushRafRef.current === null) {
        flushRafRef.current = requestAnimationFrame(flushStreamBuffer);
      }
    };

    onAppendLine(appendLine);

    return () => {
      if (flushRafRef.current !== null) {
        cancelAnimationFrame(flushRafRef.current);
        flushRafRef.current = null;
      }
      streamBufferRef.current = [];
    };
  }, [streamMode, onAppendLine, flushStreamBuffer]);

  // Update terminal content when lines change (non-stream mode)
  useEffect(() => {
    if (!xtermRef.current || streamMode) return;

    const term = xtermRef.current;
    term.clear();
    lineCountRef.current = 0;

    if (isLoading && lines.length === 0) {
      term.writeln('\x1b[33mLoading logs...\x1b[0m');
      return;
    }

    if (lines.length === 0) {
      term.writeln('\x1b[90mNo logs found matching criteria.\x1b[0m');
      return;
    }

    const buf: string[] = [];
    lines.forEach((line, index) => {
      const levelColor = getLevelColor(line.level || '');
      const lineNo = String(index + 1).padStart(6, ' ');
      const timestamp = line.timestamp || '';
      const level = (line.level || 'INFO').padEnd(7, ' ');

      buf.push(
        `\x1b[90m${lineNo}\x1b[0m `
          + `\x1b[90m[${timestamp}]\x1b[0m `
          + `${levelColor}[${level}]\x1b[0m `
          + `${line.level === 'ERROR' ? '\x1b[91m' : '\x1b[37m'}${line.message}\x1b[0m`
      );
    });

    buf.push('');
    buf.push('\x1b[90m... Waiting for new events...\r\n\x1b[0m');

    // Hide terminal during bulk write to prevent visible line-by-line scrolling
    const el = terminalRef.current;
    if (el) el.style.visibility = 'hidden';

    term.write(buf.join('\r\n'), () => {
      term.scrollToBottom();
      if (el) el.style.visibility = '';
    });

    lineCountRef.current = lines.length;
  }, [lines, isLoading, streamMode]);

  // Clear terminal when entering stream mode
  useEffect(() => {
    if (!xtermRef.current) return;

    if (streamMode) {
      xtermRef.current.clear();
      lineCountRef.current = 0;
      xtermRef.current.writeln(
        '\x1b[32m[Stream Mode] Waiting for logs...\x1b[0m'
      );
    }
  }, [streamMode]);

  return (
    <>
      <style>
        {`
        /* Subtle scrollbar styling for xterm native viewport */
        .xterm-viewport {
          scrollbar-width: thin;
          scrollbar-color: rgba(148,163,184,0.35) transparent;
        }
        .xterm-viewport::-webkit-scrollbar {
          width: 6px;
          height: 6px;
        }
        .xterm-viewport::-webkit-scrollbar-thumb {
          background: rgba(148,163,184,0.35);
          border-radius: 9999px;
          border: 2px solid transparent;
          background-clip: content-box;
        }
        .xterm-viewport::-webkit-scrollbar-thumb:hover {
          background: rgba(148,163,184,0.55);
          border: 2px solid transparent;
          background-clip: content-box;
        }
        .xterm-viewport::-webkit-scrollbar-track {
          background: transparent;
        }
      `}
      </style>

      <div ref={containerRef} className="absolute inset-0">
        <div ref={terminalRef} className="h-full w-full" />
      </div>
    </>
  );
}

function parseLogLine(rawLine: string, lineNumber: number): string {
  // 尝试解析日志行格式，例如: [2024-01-01 12:00:00] INFO: message
  const match = rawLine.match(/^\[(.+?)\]\s*(\w+):\s*(.+)$/);

  const lineNo = String(lineNumber).padStart(6, ' ');

  if (match) {
    const timestamp = match[1];
    const level = match[2].padEnd(7, ' ');
    const message = match[3];
    const levelColor = getLevelColor(match[2]);

    return (
      `\x1b[90m${lineNo}\x1b[0m `
      + `\x1b[90m[${timestamp}]\x1b[0m `
      + `${levelColor}[${level}]\x1b[0m `
      + `${match[2] === 'ERROR' ? '\x1b[91m' : '\x1b[37m'}${message}\x1b[0m`
    );
  }

  // 如果无法解析，返回原始行
  return `\x1b[90m${lineNo}\x1b[0m \x1b[37m${rawLine}\x1b[0m`;
}

function getLevelColor(level: string): string {
  switch (level) {
    case 'INFO':
      return '\x1b[36m'; // Cyan
    case 'ERROR':
      return '\x1b[91m'; // Bright Red
    case 'WARN':
      return '\x1b[93m'; // Bright Yellow
    case 'DEBUG':
      return '\x1b[90m'; // Gray
    case 'SUCCESS':
      return '\x1b[92m'; // Bright Green
    default:
      return '\x1b[37m'; // White
  }
}
