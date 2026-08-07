import { useTranslation } from 'react-i18next';
import { Label } from '@/components/ui/label';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import { Input } from '@/components/ui/input';
import { Button } from '@/components/ui/button';
import { Search, Download, X } from 'lucide-react';
import type { LogFilterOptions } from '@/types/log';

interface LogFiltersProps {
  filters: LogFilterOptions;
  onChange: (filters: LogFilterOptions) => void;
  onExport?: () => void;
}

export default function LogFilters({
  filters,
  onChange,
  onExport,
}: LogFiltersProps) {
  const { t } = useTranslation();

  const updateFilter = (key: keyof LogFilterOptions, value: any) => {
    onChange({ ...filters, [key]: value });
  };

  const clearFilters = () => {
    onChange({
      category: 'all',
      level: undefined,
      start_time: undefined,
      end_time: undefined,
      search: undefined,
      limit: 100,
      offset: 0,
    });
  };

  const hasActiveFilters =    filters.level || filters.search || filters.start_time || filters.end_time;

  return (
    <div className="flex flex-wrap items-center gap-3 p-4 bg-muted/30 rounded-lg">
      {/* Level Filter */}
      <div className="flex items-center gap-2">
        <Label className="text-sm text-muted-foreground">
          {t('sys.logs.filters.level', '级别')}
        </Label>
        <Select
          value={filters.level || 'all'}
          onValueChange={value => updateFilter('level', value === 'all' ? undefined : value)}
        >
          <SelectTrigger className="w-32 h-9">
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            <SelectItem value="all">
              {t('sys.logs.levels.all', '全部')}
            </SelectItem>
            <SelectItem value="error">
              {t('sys.logs.levels.error', '错误')}
            </SelectItem>
            <SelectItem value="warning">
              {t('sys.logs.levels.warning', '警告')}
            </SelectItem>
            <SelectItem value="info">
              {t('sys.logs.levels.info', '信息')}
            </SelectItem>
          </SelectContent>
        </Select>
      </div>

      {/* Search Input */}
      <div className="flex items-center gap-2 flex-1 min-w-[200px]">
        <div className="relative flex-1">
          <Search className="absolute left-3 top-1/2 -translate-y-1/2 w-4 h-4 text-muted-foreground" />
          <Input
            placeholder={t(
              'sys.logs.filters.search_placeholder',
              '搜索日志内容...'
            )}
            value={filters.search || ''}
            onChange={e => updateFilter('search', e.target.value)}
            className="pl-9 h-9"
          />
        </div>
        {filters.search && (
          <Button
            variant="ghost"
            size="icon-sm"
            onClick={() => updateFilter('search', undefined)}
            className="h-9 w-9"
          >
            <X className="w-4 h-4" />
          </Button>
        )}
      </div>

      {/* Actions */}
      <div className="flex items-center gap-2 ml-auto">
        {hasActiveFilters && (
          <Button
            variant="ghost"
            size="sm"
            onClick={clearFilters}
            className="h-9"
          >
            {t('common.clear', '清除')}
          </Button>
        )}
        {onExport && (
          <Button
            variant="outline"
            size="sm"
            onClick={onExport}
            className="h-9 gap-2"
          >
            <Download className="w-4 h-4" />
            {t('common.export', '导出')}
          </Button>
        )}
      </div>
    </div>
  );
}
