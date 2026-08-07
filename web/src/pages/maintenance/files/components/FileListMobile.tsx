import { FileIcon, FileTypeBadge, formatDate } from './FileIcon';
import { Checkbox } from '@/components/ui/checkbox';
import { useTranslation } from 'react-i18next';
import { formatBytes } from '../hooks/useFiles';
import type { FileInfo } from '../hooks/useFiles';
import Loading from '@/components/loading';

interface FileListMobileProps {
  files: FileInfo[];
  isLoading: boolean;
  searchText: string;
  selectedPaths: Set<string>;
  allCurrentSelected: boolean;
  onToggleSelectAll: () => void;
  onToggleSelect: (path: string) => void;
  onFileClick: (file: FileInfo) => void;
  onFileDoubleClick?: (file: FileInfo) => void;
}

export function FileListMobile({
  files,
  isLoading,
  searchText,
  selectedPaths,
  allCurrentSelected,
  onToggleSelectAll,
  onToggleSelect,
  onFileClick,
  onFileDoubleClick,
}: FileListMobileProps) {
  const { t } = useTranslation();

  const handleItemClick = (file: FileInfo) => {
    onFileClick(file);
  };

  const handleItemDoubleClick = (file: FileInfo) => {
    if (onFileDoubleClick) {
      onFileDoubleClick(file);
    }
  };

  return (
    <div className="flex-1 overflow-auto">
      {/* Select All Header */}
      {!isLoading && files.length > 0 && (
        <div className="flex items-center gap-3 px-4 py-3 border-b border-border/30 bg-muted/20 sticky top-0 z-10">
          <Checkbox
            checked={allCurrentSelected}
            onCheckedChange={onToggleSelectAll}
            id="select-all-mobile"
          />
          <label
            htmlFor="select-all-mobile"
            className="text-sm font-medium cursor-pointer select-none"
          >
            {t('sys.file_management.select_all', '全选')}
          </label>
        </div>
      )}

      {/* Loading State */}
      {isLoading ? (
        <Loading fullHeight={false} className="py-16" size="lg" />
      ) : files.length === 0 ? (
        <div className="flex flex-col items-center justify-center py-16 text-muted-foreground">
          <span className="text-sm">
            {searchText
              ? t('sys.file_management.no_results', '没有匹配的文件')
              : t('sys.file_management.empty_dir', '当前目录为空')}
          </span>
        </div>
      ) : (
        <div className="divide-y divide-border/30">
          {files.map(file => (
            <div
              key={file.path}
              className={`flex items-start gap-3 px-4 py-3.5 transition-colors hover:bg-muted/30 cursor-pointer select-none active:bg-muted/50 ${
                selectedPaths.has(file.path) ? 'bg-muted/40' : ''
              }`}
              onClick={() => handleItemClick(file)}
              onDoubleClick={() => handleItemDoubleClick(file)}
            >
              {/* Checkbox */}
              <div
                className="pt-0.5"
                onClick={e => {
                  e.stopPropagation();
                  onToggleSelect(file.path);
                }}
              >
                <Checkbox checked={selectedPaths.has(file.path)} />
              </div>

              {/* File Icon */}
              <FileIcon file={file} className="h-5 w-5 shrink-0 mt-0.5" />

              {/* File Info */}
              <div className="flex-1 min-w-0">
                <div className="flex items-center gap-2 mb-1">
                  <span
                    className={`font-medium text-sm truncate ${
                      file.is_dir
                        ? 'text-foreground hover:text-[#F24A00]'
                        : 'text-foreground hover:text-primary'
                    }`}
                  >
                    {file.name}
                  </span>
                </div>

                <div className="flex flex-wrap items-center gap-x-2 gap-y-1 text-xs text-muted-foreground">
                  {/* Size */}
                  {!file.is_dir && (
                    <span className="truncate">{formatBytes(file.size)}</span>
                  )}

                  {/* Type Badge */}
                  <FileTypeBadge file={file} />

                  {/* Modified Time */}
                  <span className="truncate">{formatDate(file.mod_time)}</span>

                  {/* Permissions */}
                  <span className="truncate font-mono text-muted-foreground">
                    {file.mode || '—'}
                  </span>
                </div>
              </div>
            </div>
          ))}
        </div>
      )}
    </div>
  );
}
