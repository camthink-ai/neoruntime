import { useEffect, useRef } from 'react';
import { Download, Eye, Pencil, Trash2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import type { FileInfo } from '../hooks/useFiles';

export interface ContextMenuState {
  visible: boolean;
  x: number;
  y: number;
  file: FileInfo | null;
}

interface FileContextMenuProps {
  state: ContextMenuState;
  onClose: () => void;
  onPreview: (file: FileInfo) => void;
  onDownload: (file: FileInfo) => void;
  onRename: (file: FileInfo) => void;
  onDelete: (file: FileInfo) => void;
  isPreviewable: (filename: string) => boolean;
}

export function FileContextMenu({
  state,
  onClose,
  onPreview,
  onDownload,
  onRename,
  onDelete,
  isPreviewable,
}: FileContextMenuProps) {
  const { t } = useTranslation();
  const ref = useRef<HTMLDivElement>(null);

  // Close on outside click / scroll / Escape
  useEffect(() => {
    if (!state.visible) return;
    const handleClick = () => onClose();
    const handleKey = (e: KeyboardEvent) => e.key === 'Escape' && onClose();
    document.addEventListener('click', handleClick);
    document.addEventListener('keydown', handleKey);
    document.addEventListener('scroll', handleClick, true);
    return () => {
      document.removeEventListener('click', handleClick);
      document.removeEventListener('keydown', handleKey);
      document.removeEventListener('scroll', handleClick, true);
    };
  }, [state.visible, onClose]);

  if (!state.visible || !state.file) return null;

  const { file } = state;

  const item = (
    icon: React.ReactNode,
    label: string,
    onClick: () => void,
    danger = false
  ) => (
    <button
      className={`flex w-full items-center gap-2.5 px-3 py-2 text-sm transition-colors ${
        danger
          ? 'text-destructive hover:bg-destructive/10'
          : 'text-foreground hover:bg-muted/60'
      }`}
      onClick={e => {
        e.stopPropagation();
        onClose();
        onClick();
      }}
    >
      {icon}
      <span>{label}</span>
    </button>
  );

  return (
    <div
      ref={ref}
      style={{
        position: 'fixed',
        top: Math.min(state.y, window.innerHeight - 240),
        left: Math.min(state.x, window.innerWidth - 180),
        zIndex: 9999,
      }}
      className="min-w-[160px] max-w-[200px] rounded-lg border border-border bg-popover shadow-lg py-1
                 animate-in fade-in-0 zoom-in-95"
      onClick={e => e.stopPropagation()}
    >
      {!file.is_dir
        && isPreviewable(file.name)
        && item(
          <Eye className="h-4 w-4 text-muted-foreground shrink-0" />,
          t('sys.file_management.view_edit', '查看/修改'),
          () => onPreview(file)
        )}

      {!file.is_dir
        && item(
          <Download className="h-4 w-4 text-muted-foreground shrink-0" />,
          t('sys.file_management.download', '下载'),
          () => onDownload(file)
        )}

      {item(
        <Pencil className="h-4 w-4 text-muted-foreground shrink-0" />,
        t('sys.file_management.rename', '重命名'),
        () => onRename(file)
      )}

      <div className="my-1 h-px bg-border mx-2" />

      {item(
        <Trash2 className="h-4 w-4 shrink-0" />,
        t('sys.file_management.delete', '删除'),
        () => onDelete(file),
        true
      )}
    </div>
  );
}
