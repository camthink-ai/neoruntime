import { useEffect, type ReactNode } from 'react';
import { createPortal } from 'react-dom';
import {
  LayoutDashboard,
  AppWindow,
  Video,
  Bot,
  Settings as SettingsIcon,
  Wrench,
  Network,
  HardDrive,
  Info,
  Image as ImageIcon,
  Cable,
  FileText,
  FolderOpen,
  Terminal as TerminalIcon,
  Activity,
  Clock,
  X,
  type LucideIcon,
} from 'lucide-react';
import { useNavigate, useLocation } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { Navigation } from '@/components/ui/navigation';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import SettingsMenu from '../components/SettingsMenu';

interface MenuItem {
  key: string;
  icon: LucideIcon;
  label: string;
  children?: MenuItem[];
}

interface MobileMenuDrawerProps {
  open: boolean;
  onClose: () => void;
}

export default function MobileMenuDrawer({
  open,
  onClose,
}: MobileMenuDrawerProps) {
  const navigate = useNavigate();
  const location = useLocation();
  const { t } = useTranslation();

  const menuItems: MenuItem[] = [
    {
      key: '/dashboard',
      icon: LayoutDashboard,
      label: t('common.dashboard'),
    },
    {
      key: '/media',
      icon: Video,
      label: t('common.media'),
    },
    {
      key: '/image',
      icon: ImageIcon,
      label: t('common.image'),
    },
    {
      key: '/apps',
      icon: AppWindow,
      label: t('common.applications'),
    },
    {
      key: '/models',
      icon: Bot,
      label: t('common.ai_models'),
    },
    {
      key: '/peripherals',
      icon: Cable,
      label: t('common.peripherals'),
    },
    {
      key: '/settings',
      icon: SettingsIcon,
      label: t('common.settings'),
      children: [
        {
          key: '/settings/device-info',
          icon: Info,
          label: t('common.device_info'),
        },
        {
          key: '/settings/time',
          icon: Clock,
          label: t('sys.time_settings.title', '时间设置'),
        },
        {
          key: '/settings/network',
          icon: Network,
          label: t('common.network'),
        },
        {
          key: '/settings/storage',
          icon: HardDrive,
          label: t('common.storage'),
        },
      ],
    },
    {
      key: '/maintenance',
      icon: Wrench,
      label: t('common.maintenance'),
      children: [
        {
          key: '/maintenance/logs',
          icon: FileText,
          label: t('common.logs'),
        },
        {
          key: '/maintenance/files',
          icon: FolderOpen,
          label: t('common.file_management'),
        },
        {
          key: '/maintenance/terminal',
          icon: TerminalIcon,
          label: t('common.terminal'),
        },
        {
          key: '/maintenance/processes',
          icon: Activity,
          label: t('maintenance.processes.title'),
        },
      ],
    },
  ];

  const handleMenuClick = (key: string) => {
    navigate(key);
    onClose();
  };

  return (
    <AnimatedMobileDrawer open={open} onClose={onClose}>
      <div className="flex h-full flex-col bg-background">
        <div className="flex items-center justify-between px-4 py-3 border-b border-border">
          <span className="text-sm font-semibold">{t('common.menu')}</span>
          <Button
            type="button"
            variant="ghost"
            size="icon-sm"
            onClick={onClose}
            aria-label={t('common.close')}
          >
            <X className="h-5 w-5" />
          </Button>
        </div>

        <div className="flex-1 overflow-auto p-4">
          <Navigation
            items={menuItems}
            selectedKey={location.pathname}
            onClick={handleMenuClick}
          />
        </div>

        <div className="flex shrink-0 justify-end border-t border-border bg-background p-4">
          <SettingsMenu
            inlineTrigger
            collapsed={false}
            side="top"
            align="end"
          />
        </div>
      </div>
    </AnimatedMobileDrawer>
  );
}

interface AnimatedMobileDrawerProps {
  open: boolean;
  onClose: () => void;
  children: ReactNode;
}

function AnimatedMobileDrawer({
  open,
  onClose,
  children,
}: AnimatedMobileDrawerProps) {
  // 简单控制 body 滚动，始终保持节点挂载，依靠 CSS 过渡做进出场动画
  useEffect(() => {
    if (open) {
      document.body.style.overflow = 'hidden';
    } else {
      document.body.style.overflow = '';
    }

    return () => {
      document.body.style.overflow = '';
    };
  }, [open]);

  const handleKeyDown = (event: React.KeyboardEvent<HTMLDivElement>) => {
    if (event.key === 'Escape') {
      onClose();
    }
  };

  return createPortal(
    <div
      className={cn(
        'fixed inset-0 z-50 transition-opacity duration-300',
        open
          ? 'opacity-100 pointer-events-auto'
          : 'opacity-0 pointer-events-none'
      )}
      aria-hidden={!open}
    >
      {/* 背景遮罩 */}
      <div
        className={cn(
          'absolute inset-0 bg-black/40 transition-opacity duration-300',
          open ? 'opacity-100' : 'opacity-0'
        )}
        role="button"
        tabIndex={0}
        aria-label="Close menu overlay"
        onClick={onClose}
        onKeyDown={handleKeyDown}
      />

      {/* 抽屉内容 */}
      <div
        className={cn(
          'absolute right-0 top-0 h-full w-72 max-w-[80vw] bg-background border-l border-border shadow-xl transition-transform duration-300 ease-out will-change-transform',
          open ? 'translate-x-0' : 'translate-x-full'
        )}
        role="dialog"
        aria-modal="true"
      >
        {children}
      </div>
    </div>,
    document.body
  );
}
