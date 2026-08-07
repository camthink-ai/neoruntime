import {
  CloudUpload,
  FolderPlus,
  RefreshCw,
  Trash2,
  Download,
} from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { ButtonGroup } from '@/components/ui/button-group';

interface ActionButtonsProps {
  selectedCount: number;
  isFetching: boolean;
  onBatchDelete: () => void;
  onBatchDownload: () => void;
  onUpload: () => void;
  onCreateFolder: () => void;
  onRefresh: () => void;
}

export function ActionButtons({
  selectedCount,
  isFetching,
  onBatchDelete,
  onBatchDownload,
  onUpload,
  onCreateFolder,
  onRefresh,
}: ActionButtonsProps) {
  const { t } = useTranslation();

  return (
    <div className="flex w-full min-w-0 flex-wrap items-center gap-2 sm:gap-3">
      <ButtonGroup>
        <Button
          variant="outline"
          size="sm"
          className="h-9"
          onClick={onBatchDownload}
          disabled={selectedCount === 0}
        >
          <Download className="h-4 w-4 mr-1" />
          <span className="hidden sm:inline">
            {selectedCount > 0
              ? t('sys.file_management.download_selected', '下载选中')
              : t('sys.file_management.download', '下载')}
          </span>
        </Button>
        <Button
          variant="outline"
          size="sm"
          className="h-9 text-destructive hover:text-destructive hover:bg-destructive/10 disabled:text-muted-foreground"
          onClick={onBatchDelete}
          disabled={selectedCount === 0}
        >
          <Trash2 className="h-4 w-4 mr-1" />
          <span className="hidden sm:inline">
            {selectedCount > 0
              ? t('sys.file_management.delete_selected', '删除选中')
              : t('sys.file_management.delete', '删除')}
          </span>
        </Button>
      </ButtonGroup>
      <Button
        variant="ghost"
        size="icon-sm"
        className="h-9 w-9 text-muted-foreground rounded-full hover:bg-muted/50 transition-colors"
        onClick={onUpload}
        title={t('sys.file_management.upload', '上传文件')}
      >
        <CloudUpload className="h-5 w-5 stroke-[1.5]" />
      </Button>
      <Button
        variant="ghost"
        size="icon-sm"
        className="h-9 w-9 text-muted-foreground rounded-full hover:bg-muted/50 transition-colors"
        onClick={onCreateFolder}
        title={t('sys.file_management.new_folder', '新建文件夹')}
      >
        <FolderPlus className="h-5 w-5 stroke-[1.5]" />
      </Button>
      <Button
        variant="ghost"
        size="icon-sm"
        className="h-9 w-9 text-muted-foreground rounded-full hover:bg-muted/50 transition-colors"
        onClick={onRefresh}
        disabled={isFetching}
        title={t('sys.file_management.refresh', '刷新')}
      >
        <RefreshCw
          className={`h-5 w-5 stroke-[1.5] ${isFetching ? 'animate-spin' : ''}`}
        />
      </Button>
    </div>
  );
}
