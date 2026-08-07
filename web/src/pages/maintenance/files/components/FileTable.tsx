import { useState, useMemo } from 'react';
import {
  useReactTable,
  getCoreRowModel,
  getSortedRowModel,
  flexRender,
  type ColumnDef,
  type SortingState,
} from '@tanstack/react-table';
import { useTranslation } from 'react-i18next';
import { useIsMobile } from '@/hooks/use-mobile';
import { cn } from '@/lib/utils';
import {
  ChevronUp,
  ChevronDown,
  ChevronsUpDown,
  Folder,
  File as FileIcon,
  Download,
  Trash2,
  Eye,
  Pencil,
  MoreHorizontal,
} from 'lucide-react';
import { Checkbox } from '@/components/ui/checkbox';
import { Button } from '@/components/ui/button';
import Loading from '@/components/loading';
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
import {
  DropdownMenu,
  DropdownMenuContent,
  DropdownMenuItem,
  DropdownMenuTrigger,
} from '@/components/ui/dropdown-menu';
import type { FileInfo } from '../hooks/useFiles';

interface FileTableProps {
  files: FileInfo[];
  isLoading: boolean;
  searchText: string;
  selectedPaths: Set<string>;
  allCurrentSelected: boolean;
  onToggleSelectAll: () => void;
  onToggleSelect: (path: string) => void;
  onFileOpen: (file: FileInfo) => void;
  onDownload: (file: FileInfo) => void;
  onRename: (file: FileInfo) => void;
  onDelete: (file: FileInfo) => void;
  isPreviewable: (name: string) => boolean;
}

export function FileTable({
  files,
  isLoading,
  searchText,
  selectedPaths,
  allCurrentSelected,
  onToggleSelectAll,
  onToggleSelect,
  onFileOpen,
  onDownload,
  onRename,
  onDelete,
  isPreviewable,
}: FileTableProps) {
  const { t } = useTranslation();
  const isMobile = useIsMobile();
  const [sorting, setSorting] = useState<SortingState>([
    { id: 'name', desc: false },
  ]);

  const formatFileSize = (bytes: number): string => {
    if (bytes === 0) return '-';
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
    if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
    return `${(bytes / (1024 * 1024 * 1024)).toFixed(1)} GB`;
  };

  const formatDate = (date: string): string => new Date(date).toLocaleString();

  const getFileType = (file: FileInfo): string => {
    if (file.is_dir) return t('sys.file_management.type_folder', '文件夹');
    const ext = file.name.split('.').pop()?.toLowerCase();
    if (!ext) return t('sys.file_management.type_file', '文件');
    return ext.toUpperCase();
  };

  const columns = useMemo<ColumnDef<FileInfo>[]>(
    () => [
      {
        id: 'select',
        header: () => (
          <Checkbox
            checked={allCurrentSelected}
            onCheckedChange={onToggleSelectAll}
            id="select-all"
          />
        ),
        cell: ({ row }) => (
          <Checkbox
            checked={selectedPaths.has(row.original.path)}
            onCheckedChange={() => onToggleSelect(row.original.path)}
          />
        ),
        size: 48,
        enableSorting: false,
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
            {t('sys.file_management.col_name', '名称')}
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
          const file = row.original;
          return (
            <div
              className={`flex items-center gap-2 ${file.is_dir ? 'cursor-pointer hover:text-primary' : ''}`}
              onClick={() => file.is_dir && onFileOpen(file)}
            >
              {file.is_dir ? (
                <Folder className="size-4 text-primary shrink-0" />
              ) : (
                <FileIcon className="size-4 text-muted-foreground shrink-0" />
              )}
              <span className="truncate font-medium">{file.name}</span>
            </div>
          );
        },
        size: 350,
      },
      {
        accessorKey: 'size',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.file_management.col_size', '大小')}
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
          <span className="font-mono text-sm text-muted-foreground">
            {formatFileSize(row.original.size)}
          </span>
        ),
        size: 120,
        sortingFn: (rowA, rowB) => rowA.original.size - rowB.original.size,
      },
      {
        id: 'type',
        accessorFn: row => getFileType(row),
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.file_management.col_type', '类型')}
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
          const type = getFileType(row.original);
          return (
            <Badge variant={row.original.is_dir ? 'default' : 'secondary'}>
              {type}
            </Badge>
          );
        },
        size: 120,
      },
      {
        accessorKey: 'mode',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.file_management.col_permissions', '权限')}
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
          const mode = row.original.mode || '-';
          const colorMap: Record<string, string> = {
            d: 'text-blue-500',
            r: 'text-green-600 dark:text-green-400',
            w: 'text-yellow-600 dark:text-yellow-400',
            x: 'text-red-500',
            s: 'text-purple-500',
            S: 'text-purple-500',
            t: 'text-purple-500',
            T: 'text-purple-500',
          };
          return (
            <span className="font-mono text-sm tracking-wide">
              {mode.split('').map((ch, i) => (
                <span
                  key={i}
                  className={colorMap[ch] ?? 'text-muted-foreground/40'}
                >
                  {ch}
                </span>
              ))}
            </span>
          );
        },
        size: 140,
      },
      {
        accessorKey: 'mod_time',
        header: ({ column }) => (
          <Button
            variant="ghost"
            size="xs"
            className="h-8 px-2 font-semibold"
            onClick={() => column.toggleSorting(column.getIsSorted() === 'asc')}
          >
            {t('sys.file_management.col_modified', '修改日期')}
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
          <span className="text-sm text-muted-foreground">
            {formatDate(row.original.mod_time)}
          </span>
        ),
        size: 200,
        sortingFn: (rowA, rowB) => new Date(rowA.original.mod_time).getTime()
          - new Date(rowB.original.mod_time).getTime(),
      },
      {
        id: 'actions',
        header: () => (
          <div className="text-left text-xs font-semibold md:text-sm">
            {t('sys.file_management.col_actions', '操作')}
          </div>
        ),
        cell: ({ row }) => {
          const file = row.original;
          const canPreview = !file.is_dir && isPreviewable(file.name);

          return (
            <div className="flex flex-wrap items-center gap-2 md:justify-start">
              <Button
                variant="outline"
                size="sm"
                className="inline-flex"
                onClick={() => onFileOpen(file)}
                disabled={!canPreview}
              >
                <Eye className="size-3" />
                {t('sys.file_management.preview', '预览')}
              </Button>
              <Button
                variant="destructive"
                size="sm"
                className="inline-flex"
                onClick={() => onDelete(file)}
              >
                <Trash2 className="size-3" />
                {t('sys.file_management.delete', '删除')}
              </Button>
              <DropdownMenu>
                <DropdownMenuTrigger asChild>
                  <Button
                    variant="outline"
                    size="sm"
                    className="inline-flex shrink-0"
                    aria-label={t('common.more', '更多')}
                  >
                    <MoreHorizontal className="size-4" />
                  </Button>
                </DropdownMenuTrigger>
                <DropdownMenuContent align="end">
                  {!file.is_dir && (
                    <DropdownMenuItem onClick={() => onDownload(file)}>
                      <Download className="mr-2 size-4" />
                      {t('sys.file_management.download', '下载')}
                    </DropdownMenuItem>
                  )}
                  <DropdownMenuItem onClick={() => onRename(file)}>
                    <Pencil className="mr-2 size-4" />
                    {t('sys.file_management.rename', '重命名')}
                  </DropdownMenuItem>
                </DropdownMenuContent>
              </DropdownMenu>
            </div>
          );
        },
        size: 220,
        enableSorting: false,
      },
    ],
    [
      t,
      selectedPaths,
      allCurrentSelected,
      onToggleSelectAll,
      onToggleSelect,
      onFileOpen,
      onDownload,
      onRename,
      onDelete,
      isPreviewable,
    ]
  );

  const table = useReactTable({
    data: files,
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
    sortingFns: {
      alphanumeric: (rowA, rowB, columnId) => {
        const a = rowA.getValue(columnId);
        const b = rowB.getValue(columnId);

        // Folders first
        if (rowA.original.is_dir !== rowB.original.is_dir) {
          return rowA.original.is_dir ? -1 : 1;
        }

        if (typeof a === 'string' && typeof b === 'string') {
          return a.localeCompare(b);
        }
        if (typeof a === 'number' && typeof b === 'number') {
          return a < b ? -1 : a > b ? 1 : 0;
        }

        // tanstack table getValue() returns unknown; only compare when type-safe
        return 0;
      },
    },
  });

  return (
    <ScrollArea className="h-full w-full" type="always">
      <Table className="table-fixed bg-white dark:bg-card" noWrapper>
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
          {isLoading ? (
            <TableRow>
              <TableCell colSpan={columns.length} className="h-0 p-0">
                <Loading fullHeight={false} className="min-h-[40vh]" />
              </TableCell>
            </TableRow>
          ) : table.getRowModel().rows?.length ? (
            table.getRowModel().rows.map(row => (
              <TableRow
                key={row.id}
                data-state={selectedPaths.has(row.original.path) && 'selected'}
              >
                {row.getVisibleCells().map((cell, index) => (
                  <TableCell
                    key={cell.id}
                    className={
                      index === row.getVisibleCells().length - 1
                        ? cn(
                            'bg-white dark:bg-card',
                            !isMobile && 'sticky right-0 z-0'
                          )
                        : index === 0
                          ? 'pl-4'
                          : ''
                    }
                    style={{ width: cell.column.getSize() }}
                  >
                    {flexRender(cell.column.columnDef.cell, cell.getContext())}
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
                {searchText
                  ? t('sys.file_management.no_results', '没有匹配的文件')
                  : t('sys.file_management.empty_dir', '当前目录为空')}
              </TableCell>
            </TableRow>
          )}
        </TableBody>
      </Table>
      <ScrollBar orientation="horizontal" />
    </ScrollArea>
  );
}
