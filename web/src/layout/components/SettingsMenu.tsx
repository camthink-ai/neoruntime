import {
  Ellipsis,
  FileQuestion,
  Sun,
  Moon,
  Monitor,
  Languages,
  LogOut,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useTheme } from 'next-themes';
import { changeLanguage } from '@/i18n/utils';
import { useAuthStore } from '@/store/auth';
import { Button } from '@/components/ui/button';
import SvgIcon from '@/components/svg-icon';
import { authApi } from '@/services/api/login';
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuGroup,
  DropdownMenuItem,
  DropdownMenuSeparator,
  DropdownMenuTrigger,
  DropdownMenuSub,
  DropdownMenuSubContent,
  DropdownMenuSubTrigger,
} from '@/components/ui/dropdown-menu';
import { cn } from '@/lib/utils';

interface SettingsMenuProps {
  collapsed?: boolean;
  side?: 'top' | 'right' | 'bottom' | 'left';
  align?: 'start' | 'center' | 'end';
  /** Trigger only as wide as content (e.g. bottom-right in mobile drawer) */
  inlineTrigger?: boolean;
}

export default function SettingsMenu({
  collapsed = false,
  side = 'right',
  align = 'end',
  inlineTrigger = false,
}: SettingsMenuProps) {
  const { t, i18n } = useTranslation();
  const { theme, setTheme } = useTheme();
  // const colorTheme = useThemeStore((s) => s.colorTheme)
  // const setColorTheme = useThemeStore((s) => s.setColorTheme)
  const clearToken = useAuthStore(s => s.clearToken);

  const handleLanguageChange = (value: string) => {
    if (value !== i18n.language) {
      changeLanguage(value);
    }
  };

  const currentTheme = theme ?? 'system';
  const langLabel = i18n.language === 'zh' ? '简体中文' : 'English';

  const getThemeLabel = (themeMode: string) => {
    switch (themeMode) {
      case 'light':
        return t('common.light_mode', 'Light Mode');
      case 'dark':
        return t('common.dark_mode', 'Dark Mode');
      default:
        return t('common.system_mode', 'System Mode');
    }
  };

  const getThemeIcon = () => {
    switch (theme) {
      case 'light':
        return Sun;
      case 'dark':
        return Moon;
      default:
        return Monitor;
    }
  };

  const ThemeIcon = getThemeIcon();

  const handleLogout = async () => {
    try {
      await authApi.logout();
    } catch {
      // 后端登出接口可能不存在或网络失败：忽略
    } finally {
      clearToken();
      window.location.href = '/login';
    }
  };

  return (
    <DropdownMenu>
      <DropdownMenuTrigger asChild>
        <Button
          variant="ghost"
          className={cn(
            inlineTrigger ? 'w-auto shrink-0' : 'w-full justify-start',
            collapsed ? 'justify-center px-0' : 'px-3',
            inlineTrigger && !collapsed && 'justify-center'
          )}
        >
          <Ellipsis className="h-4 w-4" />
          {!collapsed && <span className="ml-2">{t('common.more')}</span>}
        </Button>
      </DropdownMenuTrigger>
      <DropdownMenuContent side={side} align={align} className="w-48">
        <DropdownMenuGroup>
          <DropdownMenuSub>
            <DropdownMenuSubTrigger>
              <Languages className="h-4 w-4" />
              <span>{langLabel}</span>
            </DropdownMenuSubTrigger>
            <DropdownMenuSubContent>
              <DropdownMenuItem
                onClick={() => handleLanguageChange('zh')}
                className={cn(i18n.language === 'zh' && 'bg-accent')}
              >
                简体中文
              </DropdownMenuItem>
              <DropdownMenuItem
                onClick={() => handleLanguageChange('en')}
                className={cn(i18n.language === 'en' && 'bg-accent')}
              >
                English
              </DropdownMenuItem>
            </DropdownMenuSubContent>
          </DropdownMenuSub>
          <DropdownMenuSub>
            <DropdownMenuSubTrigger>
              <ThemeIcon className="h-4 w-4" />
              <span>{getThemeLabel(currentTheme)}</span>
            </DropdownMenuSubTrigger>
            <DropdownMenuSubContent>
              <DropdownMenuItem
                onClick={() => setTheme('light')}
                className={cn(currentTheme === 'light' && 'bg-accent')}
              >
                <Sun className="h-4 w-4" />
                <span>{getThemeLabel('light')}</span>
              </DropdownMenuItem>
              <DropdownMenuItem
                onClick={() => setTheme('dark')}
                className={cn(currentTheme === 'dark' && 'bg-accent')}
              >
                <Moon className="h-4 w-4" />
                <span>{getThemeLabel('dark')}</span>
              </DropdownMenuItem>
              <DropdownMenuItem
                onClick={() => setTheme('system')}
                className={cn(currentTheme === 'system' && 'bg-accent')}
              >
                <Monitor className="h-4 w-4" />
                <span>{getThemeLabel('system')}</span>
              </DropdownMenuItem>
            </DropdownMenuSubContent>
          </DropdownMenuSub>
          {/* 颜色主题选择已注释，使用默认 color2 主题 */}
          {/* <DropdownMenuSub>
            <DropdownMenuSubTrigger>
              <Palette className="h-4 w-4" />
              <span>{t(`sys.theme_settings.${colorTheme}`)}</span>
            </DropdownMenuSubTrigger>
            <DropdownMenuSubContent>
              <DropdownMenuItem
                onClick={() => setColorTheme('color1')}
                className={cn(colorTheme === 'color1' && 'bg-accent')}
              >
                <span>{t('sys.theme_settings.color1')}</span>
              </DropdownMenuItem>
              <DropdownMenuItem
                onClick={() => setColorTheme('color2')}
                className={cn(colorTheme === 'color2' && 'bg-accent')}
              >
                <span>{t('sys.theme_settings.color2')}</span>
              </DropdownMenuItem>
            </DropdownMenuSubContent>
          </DropdownMenuSub> */}
        </DropdownMenuGroup>
        <DropdownMenuSeparator />
        <DropdownMenuGroup>
          <DropdownMenuItem onClick={() => window.open('https://camthink-ai.github.io/ne503-aipc-sdks/', '_blank')}>
            <FileQuestion className="h-4 w-4" />
            <span>{t('common.documentation')}</span>
          </DropdownMenuItem>
          <DropdownMenuItem
            onClick={() => window.open('https://github.com/camthink-ai', '_blank')}
          >
            <span className="h-4 w-4">
              <SvgIcon icon="github" className="h-4 w-4" />
            </span>
            <span>GitHub</span>
          </DropdownMenuItem>
        </DropdownMenuGroup>
        <DropdownMenuSeparator />
        <DropdownMenuGroup>
          <DropdownMenuItem
            onClick={() => {
              handleLogout();
            }}
          >
            <LogOut className="h-4 w-4" />
            <span>{t('common.logout')}</span>
          </DropdownMenuItem>
        </DropdownMenuGroup>
      </DropdownMenuContent>
    </DropdownMenu>
  );
}
