import { cn } from '@/lib/utils';
import { type LucideIcon, ChevronDown, ChevronRight } from 'lucide-react';
import { useState } from 'react';
import { motion, AnimatePresence } from 'motion/react';
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu';

interface NavigationItem {
  key: string;
  icon?: LucideIcon;
  label: string;
  children?: NavigationItem[];
}

interface NavigationProps {
  items: NavigationItem[];
  selectedKey: string;
  onClick: (key: string) => void;
  className?: string;
  collapsed?: boolean;
  showLabels?: boolean;
}

export function Navigation({
  items,
  selectedKey,
  onClick,
  className,
  collapsed = false,
  showLabels,
}: NavigationProps) {
  const shouldShowLabels = showLabels ?? !collapsed;
  const [expandedKeys, setExpandedKeys] = useState<string[]>([]);

  const toggleExpand = (key: string) => {
    setExpandedKeys(prev => (prev.includes(key) ? prev.filter(k => k !== key) : [...prev, key]));
  };

  const renderItem = (item: NavigationItem, level = 0) => {
    const Icon = item.icon;
    const isSelected = item.key === selectedKey;
    const hasChildren = item.children && item.children.length > 0;
    const isExpanded = expandedKeys.includes(item.key);
    const isChildSelected = item.children?.some(
      child => child.key === selectedKey
    );

    const buttonClasses = cn(
      'flex items-center rounded-lg py-2.5 text-left transition-colors overflow-hidden min-w-0',
      collapsed ? 'justify-center gap-0 px-3 w-full' : 'justify-start gap-3',
      level > 0 ? 'pl-7 pr-3 mt-1 ml-2 w-[calc(100%-0.5rem)]' : 'px-3 w-full',
      'text-[#3F3F46] dark:text-[#D1D5DB] hover:bg-[#F24A00]/5 dark:hover:bg-accent/50',
      (isSelected || (hasChildren && isChildSelected))
        && 'bg-[#F24A00]/10 text-[#F24A00] hover:bg-[#F24A00]/10 hover:text-[#F24A00] dark:text-foreground dark:bg-accent dark:hover:bg-accent'
    );

    // Collapsed mode with children: use DropdownMenu
    if (collapsed && hasChildren) {
      return (
        <div key={item.key}>
          <DropdownMenu>
            <DropdownMenuTrigger asChild>
              <button className={buttonClasses}>
                {Icon && <Icon className={cn('shrink-0', 'h-6 w-6')} />}
              </button>
            </DropdownMenuTrigger>
            <DropdownMenuContent side="right" align="start" className="min-w-[160px]">
              {item.children!.map((child) => {
                const isChildActive = child.key === selectedKey;
                return (
                  <DropdownMenuItem
                    key={child.key}
                    onClick={() => onClick(child.key)}
                    className={cn(
                      'gap-2 cursor-pointer',
                      isChildActive
                        && 'bg-[#F24A00]/10 text-[#F24A00] dark:text-foreground dark:bg-[#4a4a4a]'
                    )}
                  >
                    <span>{child.label}</span>
                  </DropdownMenuItem>
                );
              })}
            </DropdownMenuContent>
          </DropdownMenu>
        </div>
      );
    }

    const buttonContent = (
      <button
        key={item.key}
        onClick={() => {
          if (hasChildren) {
            toggleExpand(item.key);
          } else {
            onClick(item.key);
          }
        }}
        className={buttonClasses}
      >
        {Icon && level === 0 && (
          <Icon
            className={cn(
              "shrink-0",
              collapsed ? "h-6 w-6" : "h-5 w-5",
            )}
          />
        )}
        {shouldShowLabels && !collapsed && (
          <>
            <span className="text-sm font-medium truncate flex-1 min-w-0">
              {item.label}
            </span>
            {hasChildren
              && (isExpanded ? (
                <ChevronDown className="h-4 w-4 shrink-0" />
              ) : (
                <ChevronRight className="h-4 w-4 shrink-0" />
              ))}
          </>
        )}
      </button>
    );

    return (
      <div key={item.key} className="min-w-0">
        {buttonContent}
        <AnimatePresence initial={false}>
          {hasChildren && isExpanded && !collapsed && (
            <motion.div
              initial={{ height: 0, opacity: 0 }}
              animate={{ height: 'auto', opacity: 1 }}
              exit={{ height: 0, opacity: 0 }}
              transition={{ duration: 0.3, ease: 'easeInOut' }}
              className="overflow-hidden"
            >
              <div className="mt-1">
                {item.children!.map(child => renderItem(child, level + 1))}
              </div>
            </motion.div>
          )}
        </AnimatePresence>
      </div>
    );
  };

  return (
    <nav className={cn('flex flex-col gap-1 overflow-hidden', className)}>
      {items.map(item => renderItem(item))}
    </nav>
  );
}
