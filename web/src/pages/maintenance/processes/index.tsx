import { useState, useMemo, useRef } from 'react';

import {
  useReactTable,
  getCoreRowModel,
  getSortedRowModel,
  flexRender,
  type ColumnDef,
  type SortingState,
} from '@tanstack/react-table';
import { useVirtualizer } from '@tanstack/react-virtual';
import { useQuery, useMutation, useQueryClient } from '@tanstack/react-query';
import { useTranslation } from 'react-i18next';
import { Card, CardHeader, CardTitle, CardContent } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Table,
  TableBody,
  TableCell,
  TableHead,
  TableHeader,
  TableRow,
} from '@/components/ui/table';
import { ScrollArea, ScrollBar } from '@/components/ui/scroll-area';
import { Badge } from '@/components/ui/badge';
import { TooltipProvider } from '@/components/ui/tooltip';
import { processApi } from '@/services/api/system';
import { toast } from 'sonner';
import Loading from '@/components/loading';
import { useIsMobile } from '@/hooks/use-mobile';
import { cn } from '@/lib/utils';
import {
  RefreshCw,
  XCircle,
  ChevronUp,
  ChevronDown,
  ChevronsUpDown,
  Info,
  Search,
  X,
} from 'lucide-react';
import { TruncateWithTooltip } from '@/components/truncate-with-tooltip';
import { ProcessDetailDialog } from './components/ProcessDetailDialog';
import { KillProcessDialog } from './components/KillProcessDialog';

// 自定义防抖 hook 已移除，改用 useRef + setTimeout 直接实现

interface ProcessInfo {
  pid: number;
  name: string;
  username: string;
  cpu_percent: number;
  mem_percent: number;
  mem_rss: number;
  status: string;
  cmdline: string;
}

export default function Processes() {
  const { t } = useTranslation();
  const isMobile = useIsMobile();
  const [sorting, setSorting] = useState<SortingState>([
    { id: 'cpu_percent', desc: true },
  ]);
  // inputValue: 输入框显示的值（实时）
  // searchQuery: 实际发给后端的值（防抖后）
  const [inputValue, setInputValue] = useState('');
  const [searchQuery, setSearchQuery] = useState('');
  const debounceTimer = useRef<ReturnType<typeof setTimeout> | null>(null);
  const isComposingRef = useRef(false);
  const scrollAreaRef = useRef<HTMLDivElement | null>(null);
  const [killDialogOpen, setKillDialogOpen] = useState(false);
  const [detailDialogOpen, setDetailDialogOpen] = useState(false);
  const [selectedProcess, setSelectedProcess] = useState<ProcessInfo | null>(
    null
  );
  const [killSignal, setKillSignal] = useState<'SIGTERM' | 'SIGKILL'>(
    'SIGTERM'
  );
  const queryClient = useQueryClient();

  // 触发防抖搜索
  const triggerSearch = (value: string) => {
    if (debounceTimer.current) clearTimeout(debounceTimer.current);
    debounceTimer.current = setTimeout(() => {
      setSearchQuery(value);
    }, 300);
  };

  const {
    data: processesData,
    isLoading,
    error,
  } = useQuery({
    queryKey: ['processes', searchQuery],
    queryFn: async () => {
      // 直接取全量，用虚拟列表渲染避免性能问题
      // Backend accepts limit 1–500; values outside that range fall back to 50
      const response = await processApi.listProcesses(
        'cpu',
        500,
        searchQuery || undefined
      );
      const data = response?.data;
      return {
        total: data?.total ?? 0,
        processes: Array.isArray(data?.processes) ? data.processes : [],
      };
    },
    refetchInterval: 5000,
  });

  const killMutation = useMutation({
    mutationFn: ({
      pid,
      signal,
    }: {
      pid: number;
      signal: 'SIGTERM' | 'SIGKILL' | 'SIGINT' | 'SIGHUP';
    }) => processApi.killProcess(pid, signal),
    onSuccess: () => {
      toast.success(
        t(
          'maintenance.processes.kill_success',
          'Process terminated successfully'
        )
      );
      setKillDialogOpen(false);
      queryClient.invalidateQueries({ queryKey: ['processes'] });
    },
    onError: (err: any) => {
      toast.error(
        `${t(
          'maintenance.processes.kill_error',
          'Failed to terminate process'
        )}: ${err?.response?.data?.message || err?.message || 'Unknown error'}`
      );
    },
  });

  const handleDetailClick = (process: ProcessInfo) => {
    setSelectedProcess(process);
    setDetailDialogOpen(true);
  };

  const handleKillClick = (
    process: ProcessInfo,
    signal: 'SIGTERM' | 'SIGKILL' = 'SIGTERM'
  ) => {
    setSelectedProcess(process);
    setKillSignal(signal);
    setKillDialogOpen(true);
  };

  const handleConfirmKill = () => {
    if (selectedProcess) {
      killMutation.mutate({ pid: selectedProcess.pid, signal: killSignal });
    }
  };

  const columns = useMemo<ColumnDef<ProcessInfo>[]>(
    () => [
      {
        accessorKey: 'pid',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.pid', 'PID')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => (
          <span className="font-mono">{row.getValue('pid')}</span>
        ),
        size: 88,
      },
      {
        accessorKey: 'name',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.name', 'Name')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const value = row.getValue('name') as string;
          return (
            <TruncateWithTooltip
              value={value}
              className="font-medium"
              tooltipClassName="max-w-md"
            >
              <span className="font-medium">{value}</span>
            </TruncateWithTooltip>
          );
        },
        size: 180,
      },
      {
        accessorKey: 'username',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.user', 'User')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const value = row.getValue('username') as string;
          return (
            <TruncateWithTooltip
              value={value}
              className="text-muted-foreground"
              tooltipClassName="max-w-md"
            >
              <span className="text-muted-foreground">{value}</span>
            </TruncateWithTooltip>
          );
        },
        size: 140,
      },
      {
        accessorKey: 'cpu_percent',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.cpu', 'CPU %')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const value = row.getValue('cpu_percent') as number;
          return (
            <span
              className={`font-mono ${
                value > 50
                  ? 'text-orange-600 dark:text-orange-400 font-semibold'
                  : ''
              }`}
            >
              {value.toFixed(1)}%
            </span>
          );
        },
        size: 96,
      },
      {
        accessorKey: 'mem_percent',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.memory', 'Memory %')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const value = row.getValue('mem_percent') as number;
          return (
            <span
              className={`font-mono ${
                value > 50
                  ? 'text-orange-600 dark:text-orange-400 font-semibold'
                  : ''
              }`}
            >
              {value.toFixed(1)}%
            </span>
          );
        },
        size: 96,
      },
      {
        accessorKey: 'mem_rss',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.mem_rss', 'RSS')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const bytes = row.getValue('mem_rss') as number;
          let formatted = `${bytes} B`;
          if (bytes >= 1024 * 1024 * 1024) {
            formatted = `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
          } else if (bytes >= 1024 * 1024) {
            formatted = `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
          } else if (bytes >= 1024) {
            formatted = `${(bytes / 1024).toFixed(1)} KB`;
          }
          return <span className="font-mono text-sm">{formatted}</span>;
        },
        size: 120,
      },
      {
        accessorKey: 'status',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.status', 'Status')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const status = row.getValue('status') as string;
          const getVariant = () => {
            const statusLower = status.toLowerCase();
            if (statusLower.includes('running')) return 'default';
            if (
              statusLower.includes('sleeping')
              || statusLower.includes('idle')
            ) return 'secondary';
            if (statusLower.includes('zombie') || statusLower.includes('dead')) return 'destructive';
            return 'outline';
          };
          return <Badge variant={getVariant()}>{status}</Badge>;
        },
        size: 80,
      },
      {
        accessorKey: 'cmdline',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('maintenance.processes.command', 'Command')}
            {column.getIsSorted() === 'asc' ? (
              <ChevronUp className="ml-2 size-4" />
            ) : column.getIsSorted() === 'desc' ? (
              <ChevronDown className="ml-2 size-4" />
            ) : (
              <ChevronsUpDown className="ml-2 size-4 opacity-50" />
            )}
          </Button>
        ),
        cell: ({ row }) => {
          const value = row.getValue('cmdline') as string;
          return (
            <TruncateWithTooltip
              value={value}
              className="font-mono text-xs"
              tooltipClassName="max-w-2xl"
              tooltipContentClassName="whitespace-pre-wrap break-all"
            >
              <span className="font-mono text-xs">{value}</span>
            </TruncateWithTooltip>
          );
        },
        size: 380,
      },
      {
        id: 'actions',
        header: () => (
          <div className="text-start">
            {t('maintenance.processes.actions', 'Actions')}
          </div>
        ),
        cell: ({ row }) => (
          <div className="flex items-center justify-start gap-2">
            <Button
              variant="outline"
              size="sm"
              onClick={() => handleDetailClick(row.original)}
            >
              <Info className="size-3" />
              {t('maintenance.processes.detail', 'Detail')}
            </Button>
            <Button
              variant="destructive"
              size="sm"
              onClick={() => handleKillClick(row.original, 'SIGTERM')}
              disabled={killMutation.isPending}
            >
              <XCircle className="size-3" />
              {t('maintenance.processes.kill', 'Kill')}
            </Button>
          </div>
        ),
        size: 180,
      },
    ],
    [t, killMutation.isPending]
  );

  const table = useReactTable({
    data: processesData?.processes || [],
    columns,
    state: {
      sorting,
    },
    onSortingChange: setSorting,
    getCoreRowModel: getCoreRowModel(),
    getSortedRowModel: getSortedRowModel(),
    enableSorting: true,
    enableMultiSort: false,
    columnResizeMode: 'onChange',
  });

  const { rows } = table.getRowModel();
  const rowVirtualizer = useVirtualizer({
    count: rows.length,
    estimateSize: () => 44,
    getScrollElement: () => {
      const root = scrollAreaRef.current;
      if (!root) return null;
      return root.querySelector(
        '[data-slot="scroll-area-viewport"]'
      ) as HTMLElement | null;
    },
    overscan: 10,
  });

  const virtualItems = rowVirtualizer.getVirtualItems();
  const paddingTop =    virtualItems.length > 0 ? (virtualItems[0]?.start ?? 0) : 0;
  const paddingBottom =    virtualItems.length > 0
      ? rowVirtualizer.getTotalSize()
        - (virtualItems[virtualItems.length - 1]?.end ?? 0)
      : 0;

  return (
    <TooltipProvider delayDuration={200}>
      <div className="flex h-full min-h-0 flex-col gap-6 p-4 md:p-6">
        <Card className="flex h-full min-h-0 flex-1 flex-col">
          <CardHeader className="border-b py-6">
            <div className="flex flex-col gap-4 sm:flex-row sm:items-center sm:justify-between">
              <CardTitle>
                {t('maintenance.processes.list', 'Running Processes')}
                {processesData && (
                  <span className="ml-2 text-sm font-normal text-muted-foreground">
                    ({processesData.total}{' '}
                    {t('maintenance.processes.total', 'total')})
                  </span>
                )}
              </CardTitle>
              <div className="flex flex-wrap items-center gap-2">
                <div className="relative">
                  <Search className="absolute left-3 top-1/2 size-4 -translate-y-1/2 text-muted-foreground" />
                  <Input
                    placeholder={t(
                      'maintenance.processes.search_placeholder',
                      'Search by name, PID, user, command...'
                    )}
                    value={inputValue}
                    onChange={e => {
                      const val = e.target.value;
                      setInputValue(val);
                      // composing 期间（输入拼音中）不触发搜索
                      if (!isComposingRef.current) {
                        triggerSearch(val);
                      }
                    }}
                    onCompositionStart={() => {
                      isComposingRef.current = true;
                    }}
                    onCompositionEnd={e => {
                      isComposingRef.current = false;
                      const val = (e.target as HTMLInputElement).value;
                      setInputValue(val);
                      triggerSearch(val);
                    }}
                    className="w-[280px] pl-9 pr-8"
                  />
                  {inputValue && (
                    <Button
                      variant="ghost"
                      size="icon-xs"
                      className="absolute right-1 top-1/2 size-6 -translate-y-1/2 text-muted-foreground hover:text-foreground"
                      onClick={() => {
                        setInputValue('');
                        setSearchQuery('');
                        if (debounceTimer.current) clearTimeout(debounceTimer.current);
                      }}
                    >
                      <X className="size-3" />
                    </Button>
                  )}
                </div>

                <Button
                  variant="outline"
                  size="icon"
                  onClick={() => queryClient.invalidateQueries({ queryKey: ['processes'] })}
                >
                  <RefreshCw className="size-4" />
                </Button>
              </div>
            </div>
          </CardHeader>
          <CardContent className="flex min-h-0 flex-1 flex-col p-0">
            <ScrollArea
              ref={scrollAreaRef}
              className="h-full w-full"
              type="always"
            >
              <Table className="table-fixed" noWrapper>
                <colgroup>
                  {columns.map((col, i) => (
                    <col key={col.id || i} style={{ width: col.size }} />
                  ))}
                </colgroup>
                <TableHeader className="sticky top-0 bg-background shadow-sm z-1">
                  {table.getHeaderGroups().map(headerGroup => (
                    <TableRow key={headerGroup.id}>
                      {headerGroup.headers.map((header, index) => (
                        <TableHead
                          key={header.id}
                          className={
                            index === headerGroup.headers.length - 1
                              ? cn(
                                  'bg-background',
                                  !isMobile
                                    && 'sticky right-0 top-0 z-10 shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.1)] dark:shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.3)]'
                                )
                              : `bg-background${index === 0 ? ' pl-4' : ''}`
                          }
                          style={{ width: header.getSize() }}
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
                  {error ? (
                    <TableRow>
                      <TableCell
                        colSpan={columns.length}
                        className="h-64 text-center"
                      >
                        <div className="flex flex-col items-center justify-center gap-2 text-destructive">
                          <XCircle className="size-8" />
                          <p className="font-semibold">
                            {t(
                              'maintenance.processes.load_error',
                              'Failed to load processes'
                            )}
                          </p>
                          <p className="text-sm text-muted-foreground">
                            {(error as any)?.message || 'Unknown error'}
                          </p>
                          <Button
                            variant="outline"
                            size="sm"
                            onClick={() => queryClient.invalidateQueries({
                                queryKey: ['processes'],
                              })}
                            className="mt-2"
                          >
                            <RefreshCw className="size-4 mr-2" />
                            {t('common.retry', 'Retry')}
                          </Button>
                        </div>
                      </TableCell>
                    </TableRow>
                  ) : isLoading ? (
                    <TableRow>
                      <TableCell colSpan={columns.length} className="h-64">
                        <Loading />
                      </TableCell>
                    </TableRow>
                  ) : rows?.length ? (
                    <>
                      {paddingTop > 0 && (
                        <TableRow>
                          <TableCell
                            colSpan={columns.length}
                            style={{ height: paddingTop }}
                          />
                        </TableRow>
                      )}
                      {virtualItems.map(virtualRow => {
                        const row = rows[virtualRow.index];
                        return (
                          <TableRow
                            key={row.id}
                            data-state={row.getIsSelected() && 'selected'}
                            ref={rowVirtualizer.measureElement}
                          >
                            {row.getVisibleCells().map((cell, index) => (
                              <TableCell
                                key={cell.id}
                                className={
                                  index === row.getVisibleCells().length - 1
                                    ? cn(
                                        'text-start bg-white dark:bg-background',
                                        !isMobile && 'sticky right-0 z-0'
                                      )
                                    : index === 0
                                      ? 'pl-4'
                                      : ''
                                }
                                style={{ width: cell.column.getSize() }}
                              >
                                {flexRender(
                                  cell.column.columnDef.cell,
                                  cell.getContext()
                                )}
                              </TableCell>
                            ))}
                          </TableRow>
                        );
                      })}
                      {paddingBottom > 0 && (
                        <TableRow>
                          <TableCell
                            colSpan={columns.length}
                            style={{ height: paddingBottom }}
                          />
                        </TableRow>
                      )}
                    </>
                  ) : (
                    <TableRow>
                      <TableCell
                        colSpan={columns.length}
                        className="h-24 text-center text-muted-foreground"
                      >
                        {t(
                          'maintenance.processes.no_processes',
                          'No processes found'
                        )}
                      </TableCell>
                    </TableRow>
                  )}
                </TableBody>
              </Table>
              <ScrollBar orientation="horizontal" />
            </ScrollArea>
          </CardContent>
        </Card>

        <ProcessDetailDialog
          open={detailDialogOpen}
          onOpenChange={setDetailDialogOpen}
          pid={selectedProcess?.pid ?? null}
        />

        <KillProcessDialog
          open={killDialogOpen}
          onOpenChange={setKillDialogOpen}
          process={selectedProcess}
          onConfirm={handleConfirmKill}
        />
      </div>
    </TooltipProvider>
  );
}
