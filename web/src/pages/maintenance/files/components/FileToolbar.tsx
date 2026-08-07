import {
  ChevronLeft,
  ChevronRight,
  ArrowUp,
  Upload,
  FolderPlus,
  RefreshCw,
  Trash2,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';

interface FileToolbarProps {
  onBack: () => void;
  onForward: () => void;
  onUp: () => void;
  canGoBack: boolean;
  canGoForward: boolean;
  onUpload: () => void;
  onMkdir: () => void;
  onRefresh: () => void;
  isRefreshing: boolean;
  selectedCount?: number;
  onBatchDelete?: () => void;
}

export function FileToolbar({
  onBack,
  onForward,
  onUp,
  canGoBack,
  canGoForward,
  onUpload,
  onMkdir,
  onRefresh,
  isRefreshing,
  selectedCount = 0,
  onBatchDelete,
}: FileToolbarProps) {
  const { t } = useTranslation();

  return (
    <div className="flex items-center gap-2 px-4 py-3 md:px-6 md:py-4 border-b border-border/50">
      {/* Navigation - hide forward/back on mobile, keep essential buttons */}
      <div className="flex items-center gap-1">
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8"
          onClick={onBack}
          disabled={canGoBack}
          title={t('sys.file_management.back', '后退')}
        >
          <ChevronLeft className="h-4 w-4" />
        </Button>
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8 hidden md:inline-flex"
          onClick={onForward}
          disabled={canGoForward}
          title={t('sys.file_management.forward', '前进')}
        >
          <ChevronRight className="h-4 w-4" />
        </Button>
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8"
          onClick={onUp}
          title={t('sys.file_management.up', '上级目录')}
        >
          <ArrowUp className="h-4 w-4" />
        </Button>
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8"
          onClick={onRefresh}
          disabled={isRefreshing}
          title={t('sys.file_management.refresh', '刷新')}
        >
          <RefreshCw
            className={`h-4 w-4 ${isRefreshing ? 'animate-spin' : ''}`}
          />
        </Button>
      </div>

      <div className="flex-1" />

      {/* Actions - hide text on mobile */}
      <div className="flex items-center gap-1">
        {selectedCount > 0 && onBatchDelete && (
          <Button
            variant="ghost"
            size="sm"
            className="h-8 text-destructive hover:text-destructive hover:bg-destructive/10"
            onClick={onBatchDelete}
            title={t('sys.file_management.batch_delete', '批量删除')}
          >
            <Trash2 className="h-4 w-4 mr-1" />
            <span className="hidden sm:inline">
              {t('sys.file_management.delete_selected', '删除选中')} (
              {selectedCount})
            </span>
            <span className="sm:hidden">({selectedCount})</span>
          </Button>
        )}
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8"
          onClick={onUpload}
          title={t('sys.file_management.upload', '上传文件')}
        >
          <Upload className="h-4 w-4" />
        </Button>
        <Button
          variant="ghost"
          size="icon-sm"
          className="h-8 w-8"
          onClick={onMkdir}
          title={t('sys.file_management.new_folder', '新建文件夹')}
        >
          <FolderPlus className="h-4 w-4" />
        </Button>
      </div>
    </div>
  );
}
