import { Download, Edit2, Trash2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Checkbox } from '@/components/ui/checkbox';
import { Button } from '@/components/ui/button';
import { TableRow, TableCell } from '@/components/ui/table';
import { formatBytes } from '../hooks/useFiles';
import type { FileInfo } from '../hooks/useFiles';
import { FileIcon, FileTypeBadge, formatDate } from './FileIcon';

interface FileRowProps {
  file: FileInfo;
  selected: boolean;
  onSelect: () => void;
  onOpen: () => void;
  onDownload: (file: FileInfo) => void;
  onRename: (file: FileInfo) => void;
  onDelete: (file: FileInfo) => void;
  isPreviewable: (name: string) => boolean;
}

export function FileRow({
  file,
  selected,
  onSelect,
  onOpen,
  onDownload,
  onRename,
  onDelete,
  isPreviewable,
}: FileRowProps) {
  const { t } = useTranslation();
  return (
    <TableRow className={selected ? 'bg-muted/40' : ''} onDoubleClick={onOpen}>
      {/* Checkbox */}
      <TableCell className="pl-4 py-3" onDoubleClick={e => e.stopPropagation()}>
        <Checkbox checked={selected} onCheckedChange={onSelect} />
      </TableCell>

      {/* Name */}
      <TableCell className="py-3" onClick={file.is_dir ? onOpen : undefined}>
        <div className="flex items-center gap-3">
          <FileIcon file={file} className="h-6 w-6 shrink-0" />
          <span
            className={`font-medium truncate ${
              file.is_dir
                ? 'text-foreground hover:text-[#F24A00] cursor-pointer'
                : 'text-foreground hover:text-primary'
            }`}
          >
            {file.name}
          </span>
        </div>
      </TableCell>

      {/* Size */}
      <TableCell className="py-3">
        <span className="font-mono text-sm text-muted-foreground">
          {file.is_dir ? '—' : formatBytes(file.size)}
        </span>
      </TableCell>

      {/* Type */}
      <TableCell className="py-3">
        <FileTypeBadge file={file} />
      </TableCell>

      {/* Modified */}
      <TableCell className="py-3">
        <span className="font-mono text-sm text-muted-foreground whitespace-pre-line leading-snug">
          {formatDate(file.mod_time)}
        </span>
      </TableCell>

      {/* Actions */}
      <TableCell>
        <div className="flex items-center justify-center gap-1">
          {!file.is_dir && isPreviewable(file.name) && (
            <Button
              className="text-muted-foreground hover:text-primary hover:bg-primary/10 p-1.5 rounded-md transition-colors"
              onClick={e => {
                e.stopPropagation();
                onOpen();
              }}
              title={t('sys.file_management.edit', '编辑')}
            >
              <Edit2 className="h-4 w-4" />
            </Button>
          )}
          {!file.is_dir && (
            <button
              className="text-muted-foreground hover:text-primary hover:bg-primary/10 p-1.5 rounded-md transition-colors"
              onClick={e => {
                e.stopPropagation();
                onDownload(file);
              }}
              title={t('sys.file_management.download', '下载')}
            >
              <Download className="h-4 w-4" />
            </button>
          )}
          <Button
            variant="ghost"
            size="icon-sm"
            className="h-8 w-8"
            onClick={e => {
              e.stopPropagation();
              onRename(file);
            }}
            title={t('sys.file_management.rename', '重命名')}
          >
            <Edit2 className="h-4 w-4" />
          </Button>
          <Button
            variant="ghost"
            size="icon-sm"
            className="h-8 w-8"
            onClick={e => {
              e.stopPropagation();
              onDelete(file);
            }}
            title={t('sys.file_management.delete', '删除')}
          >
            <Trash2 className="h-4 w-4" />
          </Button>
        </div>
      </TableCell>
    </TableRow>
  );
}
