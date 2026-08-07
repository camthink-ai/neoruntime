import type { ITheme } from 'xterm';

/** 浅色 UI 下终端仍用经典深色底 */
const BG_LIGHT_UI = '#141414';

/** 暗色 UI 下与 theme-colors 中 --card（#24211f）对齐，比纯黑底更易读 */
const BG_DARK_UI = '#1f1f1f';

const TERMINAL_COLORS_BASE: Omit<ITheme, 'background' | 'cursorAccent'> = {
  foreground: '#eaeaea',
  cursor: '#00d4ff',
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
};

/**
 * @param resolvedTheme next-themes 的 resolvedTheme；未就绪时按浅色 UI 处理
 */
export function getTerminalTheme(resolvedTheme: string | undefined): ITheme {
  const isDark = resolvedTheme === 'dark';
  const background = isDark ? BG_DARK_UI : BG_LIGHT_UI;
  return {
    ...TERMINAL_COLORS_BASE,
    background,
    cursorAccent: background,
  };
}

export const TERMINAL_BG_CLASS = 'bg-[#141414] dark:bg-[#1f1f1f]';

/** 与系统终端页一致：左/上/下留白，右侧不留 padding，滚动条贴边（配合 styles/terminal.css 中 .terminal-shell） */
export const TERMINAL_INNER_PADDING_CLASS = 'py-2.5 pl-2.5 pr-0';
