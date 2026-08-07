import { useEffect, useState } from 'react';
import { toast } from 'sonner';
import { appsApi } from '@/services/api';
import { resolveInstallApiError } from '@/pages/apps/lib/installErrorMessage';
import FileUpload from '@/components/file-upload';
import { File, CheckCircle2, X } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { useTranslation } from 'react-i18next';

interface ImageUploadProps {
  onUploadSuccess: (path: string, filename: string, size: number) => void;
  onClear?: (path?: string) => void;
  onUploadingChange?: (uploading: boolean) => void;
  initialFile?: {
    filename?: string;
    path: string;
    size?: number;
  };
}

interface UploadedFileInfo {
  filename: string;
  path: string;
  size: number;
}

function toUploadedFileInfo(
  initialFile: NonNullable<ImageUploadProps['initialFile']>,
  preservedSize?: number
): UploadedFileInfo {
  return {
    filename:
      initialFile.filename || initialFile.path.split('/').pop() || 'unknown',
    path: initialFile.path,
    size: initialFile.size ?? preservedSize ?? 0,
  };
}

function extractUploadPayload(res: unknown): {
  path?: string;
  filename?: string;
  image?: string;
  size?: number;
} | null {
  if (!res || typeof res !== 'object') return null;
  const root = res as Record<string, unknown>;
  const nested = root.data;
  if (nested && typeof nested === 'object') {
    return nested as {
      path?: string;
      filename?: string;
      image?: string;
      size?: number;
    };
  }
  return root as {
    path?: string;
    filename?: string;
    image?: string;
    size?: number;
  };
}

export default function ImageUpload({
  onUploadSuccess,
  onClear,
  onUploadingChange,
  initialFile,
}: ImageUploadProps) {
  const { t } = useTranslation();
  const [uploading, setUploading] = useState(false);
  const [progress, setProgress] = useState(0);
  const [uploadedFile, setUploadedFile] = useState<UploadedFileInfo | null>(
    initialFile ? toUploadedFileInfo(initialFile) : null
  );

  useEffect(() => {
    if (initialFile?.path) {
      setUploadedFile(prev => toUploadedFileInfo(
          initialFile,
          prev?.path === initialFile.path ? prev.size : undefined
        ));
      return;
    }
    if (!uploading) {
      setUploadedFile(null);
    }
  }, [initialFile?.path, initialFile?.filename, initialFile?.size, uploading]);

  const handleUpload = async (files: File[]) => {
    const file = files[0];

    // Validate file type
    if (
      !file.name.endsWith('.tar')
      && !file.name.endsWith('.tar.gz')
      && !file.name.endsWith('.tgz')
    ) {
      const msg = t(
        'sys.apps.import.invalid_file_type',
        '只支持 .tar、.tar.gz 或 .tgz 格式的镜像文件'
      );
      toast.error(msg);
      throw new Error(msg);
    }

    setUploading(true);
    onUploadingChange?.(true);
    setProgress(0);

    try {
      const res = await appsApi.uploadImage(file, progressValue => {
        setProgress(progressValue);
      });

      const data = extractUploadPayload(res);
      if (data?.path) {
        const imageName = data.image || data.filename || file.name;
        const fileSize = data.size ?? file.size;
        const fileInfo: UploadedFileInfo = {
          filename: data.filename || file.name,
          path: data.path,
          size: fileSize,
        };
        setUploadedFile(fileInfo);
        onUploadSuccess(data.path, imageName, fileSize);
      } else {
        throw new Error(
          t('sys.apps.import.upload_no_path', '上传成功但未返回文件路径')
        );
      }
    } catch (err: unknown) {
      const errorMsg = resolveInstallApiError(err, t);
      toast.error(errorMsg);
      throw new Error(errorMsg);
    } finally {
      setUploading(false);
      onUploadingChange?.(false);
    }
  };

  const handleReupload = () => {
    const pathToClear = uploadedFile?.path;
    setUploadedFile(null);
    setProgress(0);
    onClear?.(pathToClear);
  };

  const formatFileSize = (bytes: number): string => {
    if (!Number.isFinite(bytes) || bytes < 0) return '-';
    if (bytes < 1024) return `${bytes} B`;
    if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(2)} KB`;
    if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
    return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
  };

  // 如果已上传成功，显示文件信息
  if (uploadedFile) {
    return (
      <div className="border-2 border-dashed border-border rounded-xl p-6 bg-card">
        <div className="flex items-start gap-4">
          <div className="shrink-0 w-12 h-12 bg-green-500/10 dark:bg-green-500/20 rounded-lg flex items-center justify-center">
            <CheckCircle2 className="w-6 h-6 text-green-600 dark:text-green-500" />
          </div>
          <div className="flex-1 min-w-0">
            <div className="flex items-start justify-between gap-2 mb-2">
              <div className="flex items-center gap-2 min-w-0">
                <File className="w-4 h-4 text-muted-foreground shrink-0" />
                <span className="font-medium text-foreground break-all">
                  {uploadedFile.filename}
                </span>
              </div>
              <Button
                variant="ghost"
                size="sm"
                className="h-7 px-3 text-xs shrink-0"
                onClick={handleReupload}
              >
                <X className="w-3 h-3 mr-1" />
                {t('common.change', '更换')}
              </Button>
            </div>
            <div className="space-y-1 text-sm text-muted-foreground">
              <div className="flex items-center gap-2">
                <span className="text-xs">
                  {t('sys.apps.import.size', '大小')}:
                </span>
                <span className="font-mono">
                  {formatFileSize(uploadedFile.size)}
                </span>
              </div>
              <div className="flex items-start gap-2">
                <span className="text-xs shrink-0">
                  {t('sys.apps.import.path', '路径')}:
                </span>
                <span className="font-mono text-xs break-all">
                  {uploadedFile.path}
                </span>
              </div>
            </div>
          </div>
        </div>
      </div>
    );
  }

  return (
    <FileUpload
      single
      accept={{
        'application/x-tar': ['.tar'],
        'application/gzip': ['.tar.gz', '.tgz'],
      }}
      maxSize={2 * 1024 * 1024 * 1024} // 2GB
      placeholder={t(
        'sys.apps.import.upload_placeholder',
        '拖拽镜像文件到此处，或点击选择'
      )}
      hint={t(
        'sys.apps.import.upload_hint',
        '支持 .tar、.tar.gz、.tgz 格式，最大 2GB'
      )}
      onUpload={handleUpload}
      loading={uploading}
      showProgress
      progress={progress}
      showFileList={false}
    />
  );
}
