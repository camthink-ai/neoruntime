import { Menu } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import milesightDarkLogo from '@/assets/images/milesight-dark-logo.svg';
import milesightLightLogo from '@/assets/images/milesight-light-logo.png';

interface MobileHeaderProps {
  onMenuClick: () => void;
}

export default function MobileHeader({ onMenuClick }: MobileHeaderProps) {
  const { t } = useTranslation();

  return (
    <header className="fixed top-0 left-0 right-0 z-40 border-b border-border bg-background/95 backdrop-blur supports-backdrop-filter:bg-background/80 pt-[env(safe-area-inset-top,0px)]">
      <div className="flex h-14 shrink-0 items-center justify-between px-4">
        <div className="flex min-w-0 items-center">
          <img
            src={milesightLightLogo}
            alt="Milesight"
            className="h-7 w-auto max-w-[140px] object-contain dark:hidden"
          />
          <img
            src={milesightDarkLogo}
            alt="Milesight"
            className="hidden h-7 w-auto max-w-[140px] object-contain dark:block"
          />
        </div>
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
