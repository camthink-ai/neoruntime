import { useState, useRef, useEffect } from 'react';
import { useTranslation } from 'react-i18next';
import { debounce } from 'throttle-debounce';
import { Search } from 'lucide-react';
import { Input } from '@/components/ui/input';
import { Label } from '@/components/ui/label';
import { DateTimePicker } from '@/components/ui/datetime-picker';
import EventLogTable from './components/EventLogTable';
import type { EventFilters } from '@/types/event_log';

import { useQuery } from '@tanstack/react-query';
import { eventLogsApi } from '@/services/api/event_logs';
import { useEventTemplates } from './hooks/useLogs';
import { Separator } from '@/components/ui/separator';
import Loading from '@/components/loading';
import ErrorState from '@/components/ErrorState';

// 自定义防抖 hook
function useDebouncedValue<T>(value: T, delay: number): T {
  const [debouncedValue, setDebouncedValue] = useState(value);
  const debouncedSetRef = useRef<((v: T) => void) & { cancel: () => void }>(
    undefined);

  useEffect(() => {
    debouncedSetRef.current = debounce(delay, (v: T) => setDebouncedValue(v));
    return () => debouncedSetRef.current?.cancel();
  }, [delay]);

  useEffect(() => {
    debouncedSetRef.current?.(value);
  }, [value]);

  return debouncedValue;
}

function dateToRFC3339(value: Date | undefined): string | undefined {
  if (!value) return undefined;
  return value.toISOString();
}

export default function OperationLogs() {
  const { t } = useTranslation();
  const { data: templates = {} } = useEventTemplates();
  const [filters, setFilters] = useState<EventFilters>({
    category: 'operation',
    limit: 50,
    offset: 0,
  });
  const [currentPage, setCurrentPage] = useState(1);
  const [search, setSearch] = useState('');
  const debouncedSearch = useDebouncedValue(search, 300);
  const [startTime, setStartTime] = useState<Date | undefined>(undefined);
  const [endTime, setEndTime] = useState<Date | undefined>(undefined);

  // 当搜索/时间筛选变化时更新过滤器
  useEffect(() => {
    setFilters(prev => ({
      ...prev,
      search: debouncedSearch || undefined,
      start_time: dateToRFC3339(startTime),
      end_time: dateToRFC3339(endTime),
      offset: 0,
    }));
    setCurrentPage(1);
  }, [debouncedSearch, startTime, endTime]);

  const handlePageChange = (page: number) => {
    const newOffset = (page - 1) * filters.limit!;
    setFilters({
      ...filters,
      offset: newOffset,
    });
    setCurrentPage(page);
  };

  const handlePageSizeChange = (pageSize: number) => {
    setFilters({
      ...filters,
      limit: pageSize,
      offset: 0,
    });
    setCurrentPage(1);
  };

  return (
    <div className="flex flex-col h-full overflow-auto">
      {/* Filters */}
      <div className="px-6 py-6">
        <div className="max-w-full mx-auto">
          <div className="flex flex-wrap items-center gap-5">
            <div className="relative">
              <Search className="absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
              <Input
                type="text"
                placeholder={t('sys.event_logs.filter.search', '搜索事件...')}
                value={search}
                onChange={e => setSearch(e.target.value)}
                className="w-[280px] pl-9"
              />
            </div>

            <div className="flex items-center gap-2">
              <Label className="text-sm text-muted-foreground">
                {t('sys.event_logs.filter.start_time', '开始时间')}
              </Label>
              <DateTimePicker
                clearable
                value={startTime}
                onChange={date => setStartTime(date)}
                granularity="minute"
                placeholder={t('sys.event_logs.filter.start_time', '开始时间')}
                className="w-[200px]"
              />
            </div>

            <div className="flex items-center gap-2">
              <Label className="text-sm text-muted-foreground">
                {t('sys.event_logs.filter.end_time', '结束时间')}
              </Label>
              <DateTimePicker
                clearable
                value={endTime}
                onChange={date => setEndTime(date)}
                granularity="minute"
                placeholder={t('sys.event_logs.filter.end_time', '结束时间')}
                className="w-[200px]"
              />
            </div>
          </div>
        </div>
      </div>
      <Separator />

      {/* Table */}
      <div className="flex-1 min-h-0 pt-6">
        <div className="max-w-full mx-auto h-full">
          <EventList
            filters={filters}
            currentPage={currentPage}
            templates={templates}
            onPageChange={handlePageChange}
            onPageSizeChange={handlePageSizeChange}
          />
        </div>
      </div>
    </div>
  );
}

interface EventListProps {
  filters: EventFilters;
  currentPage: number;
  templates: Record<string, any>;
  onPageChange: (page: number) => void;
  onPageSizeChange: (pageSize: number) => void;
}

function EventList({
  filters,
  currentPage,
  templates,
  onPageChange,
  onPageSizeChange,
}: EventListProps) {
  const { t } = useTranslation();

  const { data, isLoading, isError, error } = useQuery({
    queryKey: ['event-logs', filters],
    queryFn: async () => {
      const response = await eventLogsApi.list(filters);
      return response.data;
    },
    refetchInterval: 30000,
    retry: false,
  });

  if (isLoading) {
    return (
      <table className="w-full text-sm text-left">
        <thead className="bg-muted/30 border-b border-border text-muted-foreground font-medium">
          <tr>
            <th className="px-6 py-4 font-medium">
              {t('sys.event_logs.filter.level', '级别')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.event_logs.table.time', '时间')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.event_logs.table.module', '模块')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.event_logs.table.content', '内容')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.event_logs.table.user', '用户')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('common.actions', '操作')}
            </th>
          </tr>
        </thead>
        <tbody>
          <tr>
            <td colSpan={6} className="py-12">
              <Loading />
            </td>
          </tr>
        </tbody>
      </table>
    );
  }

  if (isError) {
    return (
      <div className="flex items-center justify-center h-full">
        <ErrorState
          title={t('sys.event_logs.error.load_failed', '加载事件日志失败')}
          description={(error as Error).message}
        />
      </div>
    );
  }

  const entries = data?.entries || [];
  const total = data?.total || 0;

  return (
    <EventLogTable
      entries={entries}
      total={total}
      currentPage={currentPage}
      pageSize={filters.limit || 50}
      templates={templates}
      onPageChange={onPageChange}
      onPageSizeChange={onPageSizeChange}
    />
  );
}
