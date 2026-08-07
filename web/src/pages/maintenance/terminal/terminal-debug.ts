/** Dev-only terminal I/O tracing — enable with ?terminalDebug=1 or localStorage.terminalDebug=1 */

export function isTerminalDebugEnabled(): boolean {
  if (!import.meta.env.DEV) return false;
  try {
    return (
      new URLSearchParams(window.location.search).has('terminalDebug')
      || localStorage.getItem('terminalDebug') === '1'
    );
  } catch {
    return false;
  }
}

export function formatInputForLog(data: string): string {
  const codes = [...data].map(
    ch => `U+${ch.charCodeAt(0).toString(16).padStart(4, '0')}`
  );
  return `${JSON.stringify(data)} [${codes.join(', ')}]`;
}

export function logTerminalDebug(
  tag: string,
  payload: Record<string, unknown>
): void {
  if (!isTerminalDebugEnabled()) return;
  console.log(`%c[terminal:${tag}]`, 'color:#0ea5e9;font-weight:600', payload);
}

export function installTerminalDebugBanner(): void {
  if (!isTerminalDebugEnabled()) return;
  console.info(
    '%c[terminal] debug ON — 按数字键观察 keydown / send / recv 日志\n'
      + '  • 有 send 无 recv 回显 → 后端 readLine/PTY 未处理\n'
      + '  • 无 keydown 或无 send → 前端 xterm/IME 拦截\n'
      + '  • send 含 \\x1b 转义 → 前端 normalize 未生效',
    'color:#16a34a;font-weight:bold'
  );
}
