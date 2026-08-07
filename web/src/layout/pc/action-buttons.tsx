import { useTranslation } from 'react-i18next';
import { changeLanguage } from '@/i18n/utils';
import { useTheme } from 'next-themes';
import { Sun, Moon, Monitor, ChevronDown, LogOut } from 'lucide-react';
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
  DropdownMenuSeparator,
} from '@/components/ui/dropdown-menu';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { useAuthStore } from '@/store/auth';
import { authApi } from '@/services/api/login';

export default function ActionButtons() {
  const { i18n, t } = useTranslation();
  const { theme, setTheme } = useTheme();
  const clearToken = useAuthStore(s => s.clearToken);

  const handleLanguageChange = (value: string) => {
    if (value !== i18n.language) {
      changeLanguage(value);
    }
  };

  const handleLogout = async () => {
    try {
      await authApi.logout();
    } catch {
      // 登出接口可能不存在或网络失败：仍清理本地状态并跳转
    } finally {
      clearToken();
      window.location.href = '/login';
    }
  };

  const currentTheme = theme ?? 'system';
  const langLabel = i18n.language === 'zh' ? '简体中文' : 'English';

  const getThemeIcon = () => {
    switch (theme) {
      case 'light':
        return <Sun className="h-5 w-5" />;
      case 'dark':
        return <Moon className="h-5 w-5" />;
      default:
        return <Monitor className="h-5 w-5" />;
    }
  };

  return (
    <div className="flex gap-4 items-center">
      {/* Language switch */}
      <DropdownMenu>
        <DropdownMenuTrigger asChild>
          <Button variant="outline" className="w-[110px] justify-between">
            <span>{langLabel}</span>
            <ChevronDown className="h-4 w-4 opacity-50" />
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="end">
          <DropdownMenuItem
            onClick={() => handleLanguageChange('zh')}
            className={cn(i18n.language === 'zh' && 'bg-accent', 'mb-1')}
          >
            简体中文
          </DropdownMenuItem>
          <DropdownMenuItem
            onClick={() => handleLanguageChange('en')}
            className={cn(i18n.language === 'en' && 'bg-accent')}
          >
            English
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>

      {/* Theme switch */}
      <DropdownMenu>
        <DropdownMenuTrigger asChild>
          <Button variant="ghost" size="icon" className="w-10 h-10">
            {getThemeIcon()}
          </Button>
        </DropdownMenuTrigger>
        <DropdownMenuContent align="end">
          <DropdownMenuItem
            onClick={() => setTheme('light')}
            className={cn(currentTheme === 'light' && 'bg-accent', 'mb-1')}
          >
            <Sun className="h-4 w-4" />
            <span>{t('common.light_mode')}</span>
          </DropdownMenuItem>
          <DropdownMenuItem
            onClick={() => setTheme('dark')}
            className={cn(currentTheme === 'dark' && 'bg-accent', 'mb-1')}
          >
            <Moon className="h-4 w-4" />
            <span>{t('common.dark_mode')}</span>
          </DropdownMenuItem>
          <DropdownMenuItem
            onClick={() => setTheme('system')}
            className={cn(currentTheme === 'system' && 'bg-accent', 'mb-1')}
          >
            <Monitor className="h-4 w-4" />
            <span>{t('common.system_mode')}</span>
          </DropdownMenuItem>

          <DropdownMenuSeparator />
          <DropdownMenuItem
            variant="destructive"
            onClick={() => {
              handleLogout();
            }}
          >
            <LogOut className="h-4 w-4" />
            <span>{t('common.logout')}</span>
          </DropdownMenuItem>
        </DropdownMenuContent>
      </DropdownMenu>
    </div>
  );
}
