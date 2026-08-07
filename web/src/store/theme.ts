import { create } from 'zustand';
import { setItem, getItem } from '@/utils/storage';

type Theme = 'light' | 'dark';
type ColorTheme = 'color1' | 'color2';

const THEME_STORAGE_KEY = 'theme';
const COLOR_THEME_STORAGE_KEY = 'color-theme';

interface ThemeState {
  theme: Theme;
  colorTheme: ColorTheme;
  toggleTheme: () => void;
  setTheme: (theme: Theme) => void;
  setColorTheme: (colorTheme: ColorTheme) => void;
  initTheme: () => void;
}

// 获取系统偏好
const getSystemTheme = (): Theme => {
  if (typeof window === 'undefined') return 'light';
  return window.matchMedia('(prefers-color-scheme: dark)').matches
    ? 'dark'
    : 'light';
};

// 更新 DOM dark/light
const updateDOM = (theme: Theme) => {
  if (typeof window === 'undefined') return;
  const root = document.documentElement;
  if (theme === 'dark') {
    root.classList.add('dark');
  } else {
    root.classList.remove('dark');
  }
};

// 更新 DOM color theme
const updateColorThemeDOM = (colorTheme: ColorTheme) => {
  if (typeof window === 'undefined') return;
  document.documentElement.setAttribute('data-color-theme', colorTheme);
};

export const useThemeStore = create<ThemeState>(set => ({
  theme: 'light',
  colorTheme: 'color1',

  initTheme: () => {
    const savedTheme = getItem(THEME_STORAGE_KEY) as Theme | null;
    const initialTheme =      savedTheme === 'dark' || savedTheme === 'light'
        ? savedTheme
        : getSystemTheme();

    const savedColorTheme = getItem(
      COLOR_THEME_STORAGE_KEY
    ) as ColorTheme | null;
    const initialColorTheme =      savedColorTheme === 'color1' || savedColorTheme === 'color2'
        ? savedColorTheme
        : 'color2';

    updateDOM(initialTheme);
    updateColorThemeDOM(initialColorTheme);
    set({ theme: initialTheme, colorTheme: initialColorTheme });
  },

  toggleTheme: () => {
    set(state => {
      const newTheme = state.theme === 'light' ? 'dark' : 'light';
      updateDOM(newTheme);
      setItem(THEME_STORAGE_KEY, newTheme);
      return { theme: newTheme };
    });
  },

  setTheme: (theme: Theme) => {
    updateDOM(theme);
    setItem(THEME_STORAGE_KEY, theme);
    set({ theme });
  },

  setColorTheme: (colorTheme: ColorTheme) => {
    updateColorThemeDOM(colorTheme);
    setItem(COLOR_THEME_STORAGE_KEY, colorTheme);
    set({ colorTheme });
  },
}));
