import { useState, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import {
  useReactTable,
  getCoreRowModel,
  getSortedRowModel,
  flexRender,
  type ColumnDef,
  type SortingState,
} from '@tanstack/react-table';
import { ChevronUp, ChevronDown, ChevronsUpDown, Eye } from 'lucide-react';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';
import { ScrollArea, ScrollBar } from '@/components/ui/scroll-area';
import {
  Dialog,
  DialogClose,
  DialogContent,
  DialogTitle,
  DialogDescription,
  DialogHeader,
} from '@/components/ui/dialog';
import type { EventLogEntry, EventLevel } from '@/types/event_log';
import type { EventTemplates } from '../hooks/useLogs';
import Pagination from '@/components/Pagination';

interface EventLogTableProps {
  entries: EventLogEntry[];
  total: number;
  currentPage: number;
  pageSize: number;
  templates: EventTemplates;
  onPageChange: (page: number) => void;
  onPageSizeChange: (size: number) => void;
}

const levelLabels: Record<EventLevel, { label: string; color: string }> = {
  critical: {
    label: 'CRITICAL',
    color: 'text-purple-600 dark:text-purple-400',
  },
  error: { label: 'ERROR', color: 'text-red-600 dark:text-red-400' },
  warning: { label: 'WARN', color: 'text-amber-600 dark:text-amber-400' },
  info: { label: 'INFO', color: 'text-blue-600 dark:text-blue-400' },
};

const serviceNames: Record<string, string> = {
  auth: 'sys.event_logs.modules.auth',
  network: 'sys.event_logs.modules.network',
  storage: 'sys.event_logs.modules.storage',
  time: 'sys.event_logs.modules.time',
  ssh: 'sys.event_logs.modules.ssh',
  ai: 'sys.event_logs.modules.ai',
  app: 'sys.event_logs.modules.app',
  device: 'sys.event_logs.modules.device',
  media: 'sys.event_logs.modules.media',
  system: 'sys.event_logs.modules.system',
  firmware: 'sys.event_logs.modules.firmware',
  security: 'sys.event_logs.modules.security',
  hardware: 'sys.event_logs.modules.hardware',
  platform: 'sys.event_logs.modules.platform',
};

function formatTimestamp(timestamp: string, _locale: string): string {
  const date = new Date(timestamp);
  const now = new Date();
  const year = date.getFullYear();
  const month = String(date.getMonth() + 1).padStart(2, '0');
  const day = String(date.getDate()).padStart(2, '0');
  const hours = String(date.getHours()).padStart(2, '0');
  const minutes = String(date.getMinutes()).padStart(2, '0');
  const seconds = String(date.getSeconds()).padStart(2, '0');

  if (year !== now.getFullYear()) {
    return `${year}-${month}-${day} ${hours}:${minutes}:${seconds}`;
  }
  return `${month}-${day} ${hours}:${minutes}:${seconds}`;
}

function getSourceDisplayName(
  source: string,
  t: (key: string, fallback: string) => string
): string {
  const key = serviceNames[source];
  return key ? t(key, source) : source;
}

function getModuleDisplayName(
  module: string,
  t: (key: string, fallback: string) => string
): string {
  const key = serviceNames[module];
  return key ? t(key, module) : module;
}

/**
 * Render a localized event message using API-provided templates.
 * Backend templates use {{.param}} syntax; we replace with data values.
 */
function getEventMessage(
  eventType: string,
  data: Record<string, unknown> | undefined,
  templates: EventTemplates,
  locale: string
): string {
  const tmpl = templates[eventType];
  if (!tmpl) return eventType;

  const template = locale === 'zh' && tmpl.zh ? tmpl.zh : tmpl.en;
  if (!template) return eventType;

  // Backend uses {{.param}} syntax — replace with actual values
  let msg = template;
  if (data) {
    for (const [key, value] of Object.entries(data)) {
      msg = msg.replace(
        new RegExp(`\\{\\{\\.${key}\\}\\}`, 'g'),
        String(value)
      );
    }
  }
  return msg;
}

export default function EventLogTable({
  entries,
  total,
  currentPage,
  pageSize,
  templates,
  onPageChange,
  onPageSizeChange,
}: EventLogTableProps) {
  const { t, i18n } = useTranslation();
  const [sorting, setSorting] = useState<SortingState>([
    { id: 'timestamp', desc: true },
  ]);
  const [detailEntry, setDetailEntry] = useState<EventLogEntry | null>(null);
  const locale = i18n.language === 'zh' ? 'zh' : 'en';

  const sortIcon = (column: { getIsSorted: () => false | 'asc' | 'desc' }) => {
    const sorted = column.getIsSorted();
    if (sorted === 'asc') return <ChevronUp className="ml-2 size-4" />;
    if (sorted === 'desc') return <ChevronDown className="ml-2 size-4" />;
    return <ChevronsUpDown className="ml-2 size-4 opacity-50" />;
  };

  const columns = useMemo<ColumnDef<EventLogEntry>[]>(
    () => [
      {
        accessorKey: 'level',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.event_logs.filter.level', '级别')}
            {sortIcon(column)}
          </Button>
        ),
        cell: ({ row }) => {
          const info = levelLabels[row.original.level] || levelLabels.info;
          return (
            <span className={cn('text-xs font-semibold uppercase', info.color)}>
              {info.label}
            </span>
          );
        },
        size: 60,
      },
      {
        accessorKey: 'timestamp',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.event_logs.table.time', '时间')}
            {sortIcon(column)}
          </Button>
        ),
        cell: ({ row }) => (
          <span className="text-sm text-muted-foreground">
            {formatTimestamp(row.original.timestamp, locale)}
          </span>
        ),
        size: 60,
        sortingFn: (rowA, rowB) => new Date(rowA.original.timestamp).getTime()
          - new Date(rowB.original.timestamp).getTime(),
      },
      {
        accessorKey: 'module',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.event_logs.table.module', '模块')}
            {sortIcon(column)}
          </Button>
        ),
        cell: ({ row }) => (
          <span className="text-sm text-muted-foreground">
            {getModuleDisplayName(row.original.module, t)}
          </span>
        ),
        size: 60,
      },
      {
        accessorKey: 'message',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.event_logs.table.content', '内容')}
            {sortIcon(column)}
          </Button>
        ),
        cell: ({ row }) => {
          const msg = getEventMessage(
            row.original.event_type,
            row.original.data,
            templates,
            locale
          );
          return (
            <span className="truncate block" title={msg}>
              {msg}
            </span>
          );
        },
        size: 400,
      },
      {
        accessorKey: 'user',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.event_logs.table.user', '用户')}
            {sortIcon(column)}
          </Button>
        ),
        cell: ({ row }) => (row.original.user ? (
            <span className="text-sm text-muted-foreground">
              {row.original.user}
            </span>
          ) : (
            <span className="text-muted-foreground/50">-</span>
          )),
        size: 60,
      },
      {
        id: 'actions',
        header: () => (
          <div className="px-2 text-sm font-semibold whitespace-nowrap">
            {t('common.actions', '操作')}
          </div>
        ),
        cell: ({ row }) => (
          <Button
            variant="outline"
            size="sm"
            className="h-8 shrink-0 gap-1 whitespace-nowrap px-3"
            onClick={() => setDetailEntry(row.original)}
          >
            <Eye className="size-4 shrink-0" />
            {t('common.detail', '详情')}
          </Button>
        ),
        size: 132,
        enableSorting: false,
      },
    ],
    [t, locale]
  );

  const table = useReactTable({
    data: entries,
    columns,
    state: { sorting },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    enableSorting: true,
    enableMultiSort: false,
  });

  return (
    <div className="flex flex-col h-full min-h-full">
      {/* Table */}
      <ScrollArea className="flex-1 min-h-0" type="always">
        <div className="min-w-full">
          <Table className="table-fixed" noWrapper>
            <TableHeader className="sticky top-0 bg-background z-1">
              {table.getHeaderGroups().map(headerGroup => (
                <TableRow key={headerGroup.id}>
                  {headerGroup.headers.map((header, index) => (
                    <TableHead
                      key={header.id}
                      className={cn(
                        'bg-background',
                        index === 0 && 'pl-4',
                        header.column.id === 'actions'
                          && 'max-md:!min-w-[10rem] max-md:!w-[10rem]'
                      )}
                      style={
                        header.getSize() !== 150
                          ? { width: header.getSize() }
                          : undefined
                      }
                    >
                      {header.isPlaceholder
                        ? null
                        : flexRender(
                            header.column.columnDef.header,
                            header.getContext()
                          )}
                    </TableHead>
                  ))}
                </TableRow>
              ))}
            </TableHeader>
            <TableBody>
              {table.getRowModel().rows?.length ? (
                table.getRowModel().rows.map(row => (
                  <TableRow key={row.id}>
                    {row.getVisibleCells().map((cell, index) => (
                      <TableCell
                        key={cell.id}
                        className={cn(
                          'py-2',
                          index === 0 && 'pl-4',
                          cell.column.id === 'actions'
                            && 'max-md:!min-w-[10rem] max-md:!w-[10rem] max-md:px-2'
                        )}
                      >
                        {flexRender(
                          cell.column.columnDef.cell,
                          cell.getContext()
                        )}
                      </TableCell>
                    ))}
                  </TableRow>
                ))
              ) : (
                <TableRow>
                  <TableCell
                    colSpan={columns.length}
                    className="h-24 text-center text-muted-foreground"
                  >
                    {t('sys.event_logs.empty', '暂无事件日志')}
                  </TableCell>
                </TableRow>
              )}
            </TableBody>
          </Table>
        </div>
        <ScrollBar orientation="vertical" />
        <ScrollBar orientation="horizontal" />
      </ScrollArea>

      {/* Pagination */}
      {entries.length > 0 && (
        <Pagination
          currentPage={currentPage}
          pageSize={pageSize}
          total={total}
          onPageChange={onPageChange}
          onPageSizeChange={onPageSizeChange}
        />
      )}

      {/* Detail Dialog */}
      <Dialog
        open={!!detailEntry}
        onOpenChange={open => !open && setDetailEntry(null)}
      >
        <DialogContent className="sm:max-w-[540px] w-[95vw] max-h-[85vh] flex flex-col">
          <DialogHeader className="shrink-0">
            <DialogTitle>
              {t('sys.event_logs.table.event_detail', '事件详情')}
            </DialogTitle>
            <DialogDescription className="hidden">
              Event log details
            </DialogDescription>
          </DialogHeader>
          {detailEntry && (
            <div className="space-y-3 py-2 text-sm overflow-y-auto min-h-0 flex-1">
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.time', '时间')}
                </span>
                <span className="font-mono">{detailEntry.timestamp}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.filter.level', '级别')}
                </span>
                <span
                  className={cn(
                    'font-semibold uppercase',
                    (levelLabels[detailEntry.level] || levelLabels.info).color
                  )}
                >
                  {(levelLabels[detailEntry.level] || levelLabels.info).label}
                </span>
              </div>
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.module', '模块')}
                </span>
                <span>{getSourceDisplayName(detailEntry.source, t)}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.event_id', '事件ID')}
                </span>
                <span className="font-mono">{detailEntry.event_id}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.event_type', '事件类型')}
                </span>
                <span className="font-mono">{detailEntry.event_type}</span>
              </div>
              <div className="flex justify-between">
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.user', '用户')}
                </span>
                <span>{detailEntry.user || '-'}</span>
              </div>
              <div>
                <span className="text-muted-foreground">
                  {t('sys.event_logs.table.message', '消息')}
                </span>
                <p className="mt-1 text-foreground">
                  {getEventMessage(
                    detailEntry.event_type,
                    detailEntry.data,
                    templates,
                    locale
                  )}
                </p>
              </div>
              {detailEntry.data && Object.keys(detailEntry.data).length > 0 && (
                <div>
                  <span className="text-muted-foreground">
                    {t('sys.event_logs.table.details', '详情')}
                  </span>
                  <pre className="mt-1 bg-muted p-2 rounded-md text-xs overflow-x-auto">
                    {JSON.stringify(detailEntry.data, null, 2)}
                  </pre>
                </div>
              )}
            </div>
          )}

          <div className="shrink-0 pt-3 flex justify-end">
            <DialogClose asChild>
              <Button variant="outline" onClick={() => setDetailEntry(null)}>
                {t('common.close', '关闭')}
              </Button>
            </DialogClose>
          </div>
        </DialogContent>
      </Dialog>
    </div>
  );
}
