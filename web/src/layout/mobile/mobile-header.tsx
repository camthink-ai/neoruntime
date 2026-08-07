import { Menu } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { useTheme } from 'next-themes';
import { Button } from '@/components/ui/button';
import darkLogo from '@/assets/images/dark_logo.svg';
import lightLogo from '@/assets/images/light_logo.svg';

interface MobileHeaderProps {
  onMenuClick: () => void;
}

export default function MobileHeader({ onMenuClick }: MobileHeaderProps) {
  const { t } = useTranslation();
  const { resolvedTheme } = useTheme();
  const isDark = resolvedTheme === 'dark';
  const logoSrc = isDark ? lightLogo : darkLogo;

  return (
    <header className="fixed top-0 left-0 right-0 z-40 border-b border-border bg-background/95 backdrop-blur supports-backdrop-filter:bg-background/80 pt-[env(safe-area-inset-top,0px)]">
      <div className="flex h-14 shrink-0 items-center justify-between px-4">
        <img src={logoSrc} alt="CamThink" className="h-7 w-auto" />
        <Button
          type="button"
          variant="ghost"
          size="icon"
          onClick={onMenuClick}
          aria-label={t('common.menu')}
        >
          <Menu className="h-6 w-6" />
        </Button>
      </div>
    </header>
  );
}
