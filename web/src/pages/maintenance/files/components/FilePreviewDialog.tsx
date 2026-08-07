import { useState, useEffect, useRef } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
} from '@/components/ui/dialog';
import { Button } from '@/components/ui/button';
import { Download, Maximize2, Minimize2, Save } from 'lucide-react';
import Loading from '@/components/loading';
import { filesApi } from '@/services/api';
import MonacoEditor from './MonacoEditor';
import { toast } from 'sonner';
import { cn } from '@/lib/utils';

interface FilePreviewDialogProps {
  file: { path: string; name: string; is_dir: boolean } | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

// 文件类型判断
const getFileType = (filename: string): string => {
  const ext = filename.split('.').pop()?.toLowerCase() || '';

  // 图片
  if (['jpg', 'jpeg', 'png', 'gif', 'bmp', 'webp', 'svg'].includes(ext)) {
    return 'image';
  }

  // 视频
  if (['mp4', 'webm', 'ogg', 'mov', 'avi'].includes(ext)) {
    return 'video';
  }

  // 音频
  if (['mp3', 'wav', 'ogg', 'flac', 'm4a'].includes(ext)) {
    return 'audio';
  }

  // PDF
  if (ext === 'pdf') {
    return 'pdf';
  }

  // 代码文件
  if (
    [
      'js',
      'jsx',
      'ts',
      'tsx',
      'py',
      'java',
      'c',
      'cpp',
      'h',
      'hpp',
      'go',
      'rs',
      'php',
      'rb',
      'sh',
      'bash',
      'css',
      'scss',
      'html',
      'xml',
      'json',
      'yaml',
      'yml',
      'toml',
      'ini',
      'conf',
    ].includes(ext)
  ) {
    return 'code';
  }

  // 文本文件
  if (['txt', 'md', 'log', 'csv'].includes(ext)) {
    return 'text';
  }

  return 'unknown';
};

const FILE_PREVIEW_TOO_LARGE_CODE = 1001;

function getApiErrorCode(err: unknown): number | undefined {
  const data = (err as { data?: { code?: number } })?.data;
  return data?.code;
}

export default function FilePreviewDialog({
  file,
  open,
  onOpenChange,
}: FilePreviewDialogProps) {
  const { t } = useTranslation();
  const dialogRef = useRef<HTMLDivElement>(null);
  const [loading, setLoading] = useState(false);
  const [fileUrl, setFileUrl] = useState<string>('');
  const [error, setError] = useState<string>('');
  const [isFullscreen, setIsFullscreen] = useState(false);
  const [editContent, setEditContent] = useState('');
  const [saving, setSaving] = useState(false);
  const [showSaveConfirm, setShowSaveConfirm] = useState(false);

  const fileType = file ? getFileType(file.name) : 'unknown';

  // 切换全屏（页面内全屏）
  const toggleFullscreen = () => {
    setIsFullscreen(prev => !prev);
  };

  // 对话框关闭时重置全屏状态
  useEffect(() => {
    if (!open) {
      setIsFullscreen(false);
    }
  }, [open]);

  useEffect(() => {
    if (!open || !file || file.is_dir) {
      setFileUrl('');
      setError('');
      setEditContent('');
      return;
    }

    const loadFile = async () => {
      setLoading(true);
      setError('');

      try {
        if (
          fileType === 'image'
          || fileType === 'video'
          || fileType === 'audio'
          || fileType === 'pdf'
        ) {
          // 二进制文件：下载并创建 URL
          const blob = await filesApi.download(file.path);
          const url = window.URL.createObjectURL(blob);
          setFileUrl(url);
        } else if (fileType === 'code' || fileType === 'text') {
          // 文本文件：读取内容
          const response = await filesApi.readContentSilent(file.path);
          const text = response.data?.content || '';
          setEditContent(text);
        } else {
          setError(
            t(
              'sys.file_management.preview_not_supported',
              '不支持预览此文件类型'
            )
          );
        }
      } catch (err) {
        console.error('Failed to load file:', err);
        if (getApiErrorCode(err) === FILE_PREVIEW_TOO_LARGE_CODE) {
          setError(
            t(
              'sys.file_management.text_preview_too_large',
              'File too large (max 1MB for text view)'
            )
          );
        } else {
          const data = (
            err as { data?: { message?: string; error?: { detail?: string } } }
          )?.data;
          setError(
            data?.error?.detail
              || data?.message
              || t('sys.file_management.load_failed', '加载文件失败')
          );
        }
      } finally {
        setLoading(false);
      }
    };

    loadFile();

    // 清理函数
    return () => {
      if (fileUrl) {
        window.URL.revokeObjectURL(fileUrl);
      }
    };
  }, [open, file, fileType]);

  const handleDownload = async () => {
    if (!file) return;
    try {
      const blob = await filesApi.download(file.path);
      const url = window.URL.createObjectURL(blob);
      const link = document.createElement('a');
      link.href = url;
      link.download = file.name;
      document.body.appendChild(link);
      link.click();
      document.body.removeChild(link);
      window.URL.revokeObjectURL(url);
    } catch (err) {
      console.error('Download failed:', err);
    }
  };

  const handleSave = async () => {
    if (!file) return;
    setSaving(true);
    setShowSaveConfirm(false);
    try {
      await filesApi.writeContent(file.path, editContent);
      setEditContent(editContent);
      toast.success(t('sys.file_management.save_success', '保存成功'));
    } catch (err: any) {
      console.error('Save failed:', err);
      toast.error(
        err.response?.data?.message
          || t('sys.file_management.save_failed', '保存失败')
      );
    } finally {
      setSaving(false);
    }
  };

  const renderPreview = () => {
    if (loading) {
      return <Loading size="lg" />;
    }

    if (error) {
      return (
        <div className="flex items-center justify-center h-full">
          <div className="text-destructive">{error}</div>
        </div>
      );
    }

    switch (fileType) {
      case 'image':
        return (
          <div className="flex items-center justify-center h-full bg-[#1e1e1e]">
            <img
              src={fileUrl}
              alt={file?.name}
              className="max-w-full max-h-full object-contain"
            />
          </div>
        );

      case 'video':
        return (
          <div className="flex items-center justify-center h-full bg-[#1e1e1e]">
            {/* eslint-disable-next-line jsx-a11y/media-has-caption */}
            <video
              src={fileUrl}
              controls
              controlsList="nodownload"
              className="max-w-full max-h-full"
            >
              {t(
                'sys.file_management.video_not_supported',
                '您的浏览器不支持视频播放。'
              )}
            </video>
          </div>
        );

      case 'audio':
        return (
          <div className="flex items-center justify-center h-full bg-[#1e1e1e]">
            {/* eslint-disable-next-line jsx-a11y/media-has-caption */}
            <audio src={fileUrl} controls className="w-full max-w-md">
              {t(
                'sys.file_management.audio_not_supported',
                '您的浏览器不支持音频播放。'
              )}
            </audio>
          </div>
        );

      case 'pdf':
        return (
          <div className="w-full h-full bg-[#1e1e1e]">
            <iframe
              src={fileUrl}
              className="w-full h-full border-0"
              title={file?.name}
            />
          </div>
        );

      case 'code':
      case 'text':
        return (
          <div className="relative w-full h-full bg-[#1e1e1e]">
            <div className="absolute inset-0">
              <MonacoEditor
                value={editContent}
                filename={file?.name}
                readOnly={false}
                onChange={val => setEditContent(val)}
              />
            </div>
          </div>
        );

      default:
        return (
          <div className="flex items-center justify-center h-full bg-[#1e1e1e]">
            <div className="text-muted-foreground">
              {t(
                'sys.file_management.preview_not_supported',
                '不支持预览此文件类型'
              )}
            </div>
          </div>
        );
    }
  };

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent
        ref={dialogRef}
        className={cn(
          'flex flex-col p-0 gap-0',
          isFullscreen
            ? '!fixed !inset-0 !w-[100vw] !h-[100vh] !max-w-[100vw] !rounded-none !z-[100] !top-0 !left-0 !translate-x-0 !translate-y-0 !border-0 !shadow-none !p-0'
            : '!max-w-[90vw] w-[1200px] h-[85vh]'
        )}
      >
        <DialogHeader className="border-b px-4 pb-3 pt-4 pr-12 sm:px-6 sm:pb-4 sm:pt-6 sm:pr-14">
          <div className="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between sm:gap-4">
            <DialogTitle className="min-w-0 max-w-full shrink truncate text-left text-base leading-tight sm:text-lg">
              {file?.name}
            </DialogTitle>
            <div className="flex w-full min-w-0 flex-wrap items-center gap-2 sm:w-auto sm:flex-nowrap sm:justify-end">
              {(fileType === 'code' || fileType === 'text') && (
                <>
                  <Button
                    variant="default"
                    size="sm"
                    onClick={() => setShowSaveConfirm(true)}
                    disabled={saving}
                  >
                    <Save className="w-4 h-4 mr-2" />
                    {saving
                      ? t('sys.file_management.saving', '保存中...')
                      : t('sys.file_management.save', '保存')}
                  </Button>
                  <div className="w-px h-4 bg-slate-200 mx-1" />
                </>
              )}
              <Button
                variant="outline"
                size="sm"
                onClick={handleDownload}
                title={t('sys.file_management.download_file', '下载文件')}
              >
                <Download className="w-4 h-4 mr-2" />
                {t('sys.file_management.download', '下载')}
              </Button>
              <Button
                variant="ghost"
                size="icon-sm"
                className="h-8 w-8"
                onClick={toggleFullscreen}
                title={
                  isFullscreen
                    ? t('sys.file_management.exit_fullscreen', '退出全屏')
                    : t('sys.file_management.fullscreen', '全屏')
                }
              >
                {isFullscreen ? (
                  <Minimize2 className="w-4 h-4" />
                ) : (
                  <Maximize2 className="w-4 h-4" />
                )}
              </Button>
            </div>
          </div>
        </DialogHeader>

        <div className="flex-1 overflow-hidden relative">
          {renderPreview()}

          {/* 在 Dialog 内部显示确认遮罩 */}
          {showSaveConfirm && (
            <div className="absolute inset-0 bg-black/50 flex items-center justify-center z-50">
              <div className="bg-background border border-border rounded-lg shadow-lg p-6 max-w-md w-full mx-4">
                <div className="space-y-2 mb-4">
                  <h2 className="text-lg font-semibold">
                    {t('sys.file_management.confirm_save', '确认保存')}
                  </h2>
                  <p className="text-sm text-muted-foreground">
                    {t(
                      'sys.file_management.confirm_save_description',
                      '确定要保存对文件的修改吗？此操作将覆盖原文件。'
                    )}
                  </p>
                </div>
                <div className="flex justify-end space-x-2 mt-6">
                  <Button
                    variant="outline"
                    onClick={() => setShowSaveConfirm(false)}
                    disabled={saving}
                  >
                    {t('common.cancel', '取消')}
                  </Button>
                  <Button
                    variant="carbon"
                    onClick={handleSave}
                    disabled={saving}
                  >
                    {saving
                      ? t('sys.file_management.saving', '保存中...')
                      : t('common.confirm', '确认')}
                  </Button>
                </div>
              </div>
            </div>
          )}
        </div>
      </DialogContent>
    </Dialog>
  );
}
