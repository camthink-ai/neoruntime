import { useThemeStore } from '@/store/theme';
import { useEffect } from 'react';

export function useTheme() {
  const theme = useThemeStore(state => state.theme);
  const toggleTheme = useThemeStore(state => state.toggleTheme);
  const setTheme = useThemeStore(state => state.setTheme);

  // 初始化时确保 DOM 已更新
  useEffect(() => {
    const root = document.documentElement;
    if (theme === 'dark') {
      root.classList.add('dark');
    } else {
      root.classList.remove('dark');
    }
  }, [theme]);

  return { theme, toggleTheme, setTheme };
}
