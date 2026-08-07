import { useState } from 'react';
import {
  LayoutDashboard,
  AppWindow,
  Video,
  Bot,
  Settings as SettingsIcon,
  Wrench,
  PanelLeftClose,
  PanelLeftOpen,
  Network,
  HardDrive,
  Image as ImageIcon,
  Cable,
  FileText,
  FolderOpen,
  Terminal as TerminalIcon,
  Activity,
  Clock,
  Info,
  type LucideIcon,
} from 'lucide-react';
import { useNavigate, useLocation } from 'react-router-dom';
import { useTranslation } from 'react-i18next';
import { Navigation } from '@/components/ui/navigation';
import { Button } from '@/components/ui/button';
import { cn } from '@/lib/utils';
import { getItem, setItem } from '@/utils/storage';
import { useTheme } from 'next-themes';
import darkLogo from '@/assets/images/dark_logo.svg';
import lightLogo from '@/assets/images/light_logo.svg';
import SettingsMenu from '../components/SettingsMenu';
import { ScrollArea } from '@/components/ui/scroll-area';

interface MenuItem {
  key: string;
  icon: LucideIcon;
  label: string;
  children?: MenuItem[];
}

const MENU_COLLAPSED_KEY = 'pc-menu-collapsed';

export default function PCMenu() {
  const navigate = useNavigate();
  const location = useLocation();
  const { t } = useTranslation();

  const { resolvedTheme } = useTheme();
  const isDark = resolvedTheme === 'dark';
  const logoSrc = isDark ? lightLogo : darkLogo;

  // 从 localStorage 读取初始状态，默认为展开（false）
  const [collapsed, setCollapsed] = useState(() => {
    const saved = getItem<boolean>(MENU_COLLAPSED_KEY)
    return saved ?? false
  })
  const [isHidden, setIsHidden] = useState(false)

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
  };

  const handleToggleCollapse = () => {
    setTimeout(() => {
      setCollapsed(prev => {
        const newValue = !prev;
        // 保存到 localStorage
        setItem(MENU_COLLAPSED_KEY, newValue);
        return newValue;
      });
      setIsHidden(false);
    }, 200);
    setIsHidden(true);
  };

  return (
    <aside
      className={cn(
        'h-full sticky top-0 left-0 bg-[#FEF6F2] dark:bg-[#1f1f1f] flex flex-col transition-[width,padding] duration-300 ease-in-out',
        collapsed ? 'w-[72px] px-2 py-4' : 'w-[220px] px-4 py-4'
      )}
    >
      {/* Logo + Collapse Toggle */}
      <div
        className={cn(
          'flex items-center mb-6',
          collapsed ? 'justify-center' : 'justify-between'
        )}
      >
        {!collapsed && (
          <img src={logoSrc} alt="CamThink" className="h-7 w-auto" />
        )}
        <Button
          type="button"
          size="icon-sm"
          variant="ghost"
          className={cn(
            'w-8 h-8 text-foreground hover:bg-accent',
            !collapsed && 'ml-auto'
          )}
          onClick={handleToggleCollapse}
          aria-label={collapsed ? t('common.expand') : t('common.collapse')}
        >
          {collapsed ? (
            <PanelLeftOpen className="h-4 w-4" />
          ) : (
            <PanelLeftClose className="h-4 w-4" />
          )}
        </Button>
      </div>

      <ScrollArea className="flex-1 min-h-0">
        <Navigation
          items={menuItems}
          selectedKey={location.pathname}
          onClick={handleMenuClick}
          collapsed={collapsed}
          className={cn(
            'transition-opacity duration-200 ease-in-out gap-1.5',
            isHidden ? 'opacity-0 pointer-events-none' : 'opacity-100'
          )}
        />
      </ScrollArea>

      <div className="mt-auto pt-4">
        <SettingsMenu collapsed={collapsed} side="right" align="end" />
      </div>
    </aside>
  );
}
