import { useTranslation } from 'react-i18next';
import { useThemeStore } from '@/store/theme';
import { cn } from '@/lib/utils';
import { Check } from 'lucide-react';

type ColorTheme = 'color1' | 'color2';

interface ThemePreview {
  key: ColorTheme;
  lightColors: {
    background: string;
    sidebar: string;
    accent: string;
    foreground: string;
    muted: string;
    border: string;
  };
  darkColors: {
    background: string;
    sidebar: string;
    accent: string;
    foreground: string;
    muted: string;
    border: string;
  };
}

const themes: ThemePreview[] = [
  {
    key: 'color1',
    lightColors: {
      background: '#ffffff',
      sidebar: '#050d17',
      accent: '#f24a00',
      foreground: '#050d17',
      muted: '#64748b',
      border: '#e2e8f0',
    },
    darkColors: {
      background: '#050d17',
      sidebar: '#03080e',
      accent: '#f24a00',
      foreground: '#f8fafc',
      muted: '#94a3b8',
      border: '#1a2431',
    },
  },
  {
    key: 'color2',
    lightColors: {
      background: '#fafaf9',
      sidebar: '#fdfcfb',
      accent: '#f24a00',
      foreground: '#2b2a29',
      muted: '#78716c',
      border: '#e7e5e4',
    },
    darkColors: {
      background: '#1c1917',
      sidebar: '#1c1917',
      accent: '#f24a00',
      foreground: '#fdfcfb',
      muted: '#a8a29e',
      border: '#2e2a28',
    },
  },
];

export default function ThemeSettings() {
  const { t } = useTranslation();
  const colorTheme = useThemeStore(s => s.colorTheme);
  const setColorTheme = useThemeStore(s => s.setColorTheme);

  return (
    <div className="space-y-6">
      <div>
        <h3 className="text-lg font-semibold">
          {t('sys.theme_settings.title', 'Theme')}
        </h3>
        <p className="text-sm text-muted-foreground mt-1">
          {t('sys.theme_settings.color_theme', 'Color Theme')}
        </p>
      </div>

      <div className="grid grid-cols-1 sm:grid-cols-2 gap-4">
        {themes.map(theme => {
          const isActive = colorTheme === theme.key;
          return (
            <button
              key={theme.key}
              type="button"
              onClick={() => setColorTheme(theme.key)}
              className={cn(
                'relative group rounded-xl border-2 p-3 transition-all duration-200 text-left',
                isActive
                  ? 'border-accent-foreground shadow-md'
                  : 'border-border hover:border-muted-foreground/40 hover:shadow-sm'
              )}
            >
              {/* Selected indicator */}
              {isActive && (
                <div className="absolute top-2 right-2 h-5 w-5 rounded-full bg-accent-foreground flex items-center justify-center">
                  <Check className="h-3 w-3 text-white" />
                </div>
              )}

              {/* Theme name */}
              <div className="mb-3 font-medium text-sm">
                {t(`sys.theme_settings.${theme.key}`)}
              </div>

              {/* Light mode preview */}
              <div
                className="rounded-lg overflow-hidden border mb-2"
                style={{ borderColor: theme.lightColors.border }}
              >
                <div className="flex h-16">
                  {/* Sidebar preview */}
                  <div
                    className="w-8 flex flex-col items-center gap-1.5 pt-2"
                    style={{ backgroundColor: theme.lightColors.sidebar }}
                  >
                    <div
                      className="w-3 h-3 rounded-sm"
                      style={{ backgroundColor: theme.lightColors.accent }}
                    />
                    <div className="w-3 h-0.5 rounded-full bg-white/30" />
                    <div className="w-3 h-0.5 rounded-full bg-white/20" />
                    <div className="w-3 h-0.5 rounded-full bg-white/20" />
                  </div>
                  {/* Content preview */}
                  <div
                    className="flex-1 p-2 flex flex-col gap-1"
                    style={{ backgroundColor: theme.lightColors.background }}
                  >
                    <div
                      className="h-1.5 w-12 rounded-full"
                      style={{ backgroundColor: theme.lightColors.foreground }}
                    />
                    <div
                      className="h-1 w-16 rounded-full"
                      style={{ backgroundColor: theme.lightColors.muted }}
                    />
                    <div className="flex gap-1 mt-auto">
                      <div
                        className="h-3 w-8 rounded-sm"
                        style={{ backgroundColor: theme.lightColors.accent }}
                      />
                      <div
                        className="h-3 w-8 rounded-sm"
                        style={{
                          backgroundColor: theme.lightColors.border,
                        }}
                      />
                    </div>
                  </div>
                </div>
              </div>

              {/* Dark mode preview */}
              <div
                className="rounded-lg overflow-hidden border"
                style={{ borderColor: theme.darkColors.border }}
              >
                <div className="flex h-16">
                  {/* Sidebar preview */}
                  <div
                    className="w-8 flex flex-col items-center gap-1.5 pt-2"
                    style={{ backgroundColor: theme.darkColors.sidebar }}
                  >
                    <div
                      className="w-3 h-3 rounded-sm"
                      style={{ backgroundColor: theme.darkColors.accent }}
                    />
                    <div className="w-3 h-0.5 rounded-full bg-white/30" />
                    <div className="w-3 h-0.5 rounded-full bg-white/20" />
                    <div className="w-3 h-0.5 rounded-full bg-white/20" />
                  </div>
                  {/* Content preview */}
                  <div
                    className="flex-1 p-2 flex flex-col gap-1"
                    style={{ backgroundColor: theme.darkColors.background }}
                  >
                    <div
                      className="h-1.5 w-12 rounded-full"
                      style={{ backgroundColor: theme.darkColors.foreground }}
                    />
                    <div
                      className="h-1 w-16 rounded-full"
                      style={{ backgroundColor: theme.darkColors.muted }}
                    />
                    <div className="flex gap-1 mt-auto">
                      <div
                        className="h-3 w-8 rounded-sm"
                        style={{ backgroundColor: theme.darkColors.accent }}
                      />
                      <div
                        className="h-3 w-8 rounded-sm"
                        style={{
                          backgroundColor: theme.darkColors.border,
                        }}
                      />
                    </div>
                  </div>
                </div>
              </div>

              {/* Labels under previews */}
              <div className="flex justify-between mt-2 text-xs text-muted-foreground">
                <span>{t('common.light_mode', 'Light')}</span>
                <span>{t('common.dark_mode', 'Dark')}</span>
              </div>
            </button>
          );
        })}
      </div>
    </div>
  );
}
