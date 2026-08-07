import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Layers, Shield, AlertCircle, Server } from 'lucide-react';
import { cn } from '@/lib/utils';
import type { LogCategory as LogCategoryType } from '@/types/log';

interface CategoryItem {
  key: LogCategoryType;
  icon: React.ComponentType<{ className?: string }>;
  label: string;
}

const categories: CategoryItem[] = [
  { key: 'all', icon: Layers, label: 'sys.logs.categories.all' },
  { key: 'operation', icon: Server, label: 'sys.logs.categories.operation' },
  { key: 'security', icon: Shield, label: 'sys.logs.categories.security' },
  { key: 'alarm', icon: AlertCircle, label: 'sys.logs.categories.alarm' },
  { key: 'system', icon: Server, label: 'sys.logs.categories.system' },
];

interface LogCategoriesProps {
  selected: LogCategoryType;
  onChange: (category: LogCategoryType) => void;
}

export default function LogCategories({
  selected,
  onChange,
}: LogCategoriesProps) {
  const { t } = useTranslation();

  return (
    <div className="flex gap-2 mb-4 overflow-x-auto pb-2">
      {categories.map(category => {
        const Icon = category.icon;
        const isSelected = selected === category.key;

        return (
          <Button
            key={category.key}
            variant={isSelected ? 'default' : 'outline'}
            size="sm"
            onClick={() => onChange(category.key)}
            className={cn(
              'gap-2 whitespace-nowrap',
              isSelected && 'bg-[#f24a00] hover:bg-[#e04500] text-white'
            )}
          >
            <Icon className="w-4 h-4" />
            <span>{t(category.label, category.label)}</span>
          </Button>
        );
      })}
    </div>
  );
}
