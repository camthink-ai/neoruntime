import { useCallback, useState } from 'react';
import {
  useDropzone,
  type DropzoneOptions,
  type FileRejection,
} from 'react-dropzone';
import { UploadCloud, X, File, CheckCircle2 } from 'lucide-react';
import { useTranslation } from 'react-i18next';
import { cn } from '@/lib/utils';
import { toast } from 'sonner';

export interface FileUploadProps extends Omit<DropzoneOptions, 'onDrop'> {
  className?: string;
  /** 上传成功回调 */
  onUpload?: (files: File[]) => void | Promise<void>;
  /** 文件改变回调（选择后立即触发，不等待上传） */
  onChange?: (files: File[]) => void;
  /** 单文件模式（默认 false） */
  single?: boolean;
  /** 显示已上传的文件列表 */
  showFileList?: boolean;
  /** 自定义占位文本 */
  placeholder?: string;
  /** 自定义提示文本 */
  hint?: string;
  /** 上传中状态 */
  loading?: boolean;
  /** 已上传的文件（受控模式） */
  value?: File[];
  /** 禁用状态 */
  disabled?: boolean;
  /** 自定义渲染内容 */
  children?: React.ReactNode;
  /** 显示进度条 */
  showProgress?: boolean;
  /** 上传进度（0-100） */
  progress?: number;
}

export default function FileUpload({
  className,
  onUpload,
  onChange,
  single = false,
  showFileList = true,
  placeholder,
  hint,
  loading = false,
  value,
  disabled = false,
  children,
  showProgress = false,
  progress = 0,
  accept,
  maxSize,
  maxFiles,
  ...dropzoneOptions
}: FileUploadProps) {
  const { t } = useTranslation();
  const [selectedFiles, setSelectedFiles] = useState<File[]>(value || []);
  const [isDragging, setIsDragging] = useState(false);

  const handleDrop = useCallback(
    async (acceptedFiles: File[], rejectedFiles: FileRejection[]) => {
      // 处理被拒绝的文件
      if (rejectedFiles.length > 0) {
        const error = rejectedFiles[0].errors[0];
        if (error.code === 'file-too-large') {
          toast.error(
            `文件过大，最大允许 ${maxSize ? `${(maxSize / 1024 / 1024).toFixed(1)}MB` : '未限制'}`
          );
        } else if (error.code === 'file-invalid-type') {
          toast.error('文件类型不支持');
        } else {
          toast.error(error.message);
        }
        return;
      }

      if (acceptedFiles.length === 0) return;

      // 单文件模式只取第一个
      const files = single ? [acceptedFiles[0]] : acceptedFiles;

      setSelectedFiles(files);

      // 触发 onChange
      onChange?.(files);

      // 如果提供了 onUpload，执行上传逻辑
      if (onUpload) {
        try {
          await onUpload(files);
        } catch (error) {
          console.error('Upload failed:', error);
        }
      }
    },
    [single, onChange, onUpload, maxSize]
  );

  const { getRootProps, getInputProps, isDragActive } = useDropzone({
    onDrop: handleDrop,
    onDragEnter: () => setIsDragging(true),
    onDragLeave: () => setIsDragging(false),
    accept,
    maxSize,
    maxFiles: single ? 1 : maxFiles,
    multiple: !single,
    disabled: disabled || loading,
    ...dropzoneOptions,
  });

  const removeFile = (index: number) => {
    const newFiles = selectedFiles.filter((_, i) => i !== index);
    setSelectedFiles(newFiles);
    onChange?.(newFiles);
  };

  const files = value || selectedFiles;
  const hasFiles = files.length > 0;

  // 如果提供了自定义子元素，使用自定义渲染
  if (children) {
    return (
      <div {...getRootProps()} className={cn('relative', className)}>
        <input {...getInputProps()} />
        {children}
      </div>
    );
  }

  return (
    <div className={cn('w-full', className)}>
      <div
        {...getRootProps()}
        className={cn(
          'relative flex flex-col items-center justify-center',
          'p-8 border-2 border-dashed rounded-xl cursor-pointer transition-all',
          isDragActive || isDragging
            ? 'border-primary bg-primary/10'
            : 'border-border/50 hover:border-border bg-muted/40',
          (disabled || loading)
            && 'opacity-50 cursor-not-allowed pointer-events-none',
          hasFiles && 'bg-card'
        )}
      >
        <input {...getInputProps()} />

        {/* 上传中状态 */}
        {loading && showProgress ? (
          <div className="w-full">
            <div className="w-full bg-muted/60 rounded-full h-2 mb-2">
              <div
                className="bg-primary h-2 rounded-full transition-all"
                style={{ width: `${progress}%` }}
              />
            </div>
            <p className="text-sm text-muted-foreground text-center">
              {progress}%
            </p>
          </div>
        ) : loading ? (
          <div className="flex items-center gap-3">
            <div className="w-5 h-5 border-2 border-primary border-t-transparent rounded-full animate-spin" />
            <span className="text-muted-foreground">
              {t('sys.file_upload.uploading', 'Uploading...')}
            </span>
          </div>
        ) : hasFiles ? (
          /* 已选择文件 */
          <div className="w-full space-y-3">
            {showFileList
              && files.map((file, index) => (
                <div
                  key={`${file.name}-${index}`}
                  className="flex items-center justify-between p-3 bg-muted/40 rounded-lg"
                  onClick={e => e.stopPropagation()}
                >
                  <div className="flex items-center gap-3 flex-1 min-w-0">
                    <File className="w-5 h-5 text-muted-foreground shrink-0" />
                    <div className="flex-1 min-w-0">
                      <p className="text-sm font-medium text-foreground truncate">
                        {file.name}
                      </p>
                      <p className="text-xs text-muted-foreground">
                        {(file.size / 1024).toFixed(1)} KB
                      </p>
                    </div>
                  </div>
                  <button
                    type="button"
                    onClick={e => {
                      e.stopPropagation();
                      removeFile(index);
                    }}
                    className="p-1 hover:bg-muted/60 rounded transition-colors shrink-0"
                  >
                    <X className="w-4 h-4 text-muted-foreground" />
                  </button>
                </div>
              ))}
            {!single && (
              <p className="text-sm text-center text-muted-foreground pt-2">
                {t(
                  'sys.file_upload.add_more_files',
                  'Click or drag to add more files'
                )}
              </p>
            )}
          </div>
        ) : (
          /* 空状态 */
          <>
            <UploadCloud className="w-12 h-12 mb-3 text-muted-foreground" />
            <p className="text-muted-foreground mb-1">
              {placeholder
                || t(
                  'sys.file_upload.placeholder',
                  'Drag files here, or click to select'
                )}
            </p>
            {hint && <p className="text-sm text-muted-foreground">{hint}</p>}
            {accept && (
              <p className="text-xs text-muted-foreground mt-2">
                {t('sys.file_management.supported_formats', '支持格式')}:{' '}
                {typeof accept === 'string'
                  ? accept
                  : Object.keys(accept).join(', ')}
              </p>
            )}
            {maxSize && (
              <p className="text-xs text-muted-foreground">
                {t('common.max_file_size', {
                  size: `${(maxSize / 1024 / 1024).toFixed(1)}MB`,
                  defaultValue: '最大文件大小: {{size}}',
                })}
              </p>
            )}
          </>
        )}
      </div>
    </div>
  );
}

/**
 * 简化的文件上传按钮组件
 */
interface FileUploadButtonProps extends Omit<FileUploadProps, 'children'> {
  buttonText?: string;
  buttonClassName?: string;
}

export function FileUploadButton({
  buttonText = '选择文件',
  buttonClassName,
  ...props
}: FileUploadButtonProps) {
  return (
    <FileUpload {...props}>
      <button
        type="button"
        className={cn(
          'px-4 py-2 bg-primary text-primary-foreground rounded-lg hover:bg-primary/90 transition-colors',
          buttonClassName
        )}
      >
        {buttonText}
      </button>
    </FileUpload>
  );
}

/**
 * 图片上传预览组件
 */
interface ImageUploadProps extends FileUploadProps {
  previewUrl?: string;
}

export function ImageUpload({ previewUrl, ...props }: ImageUploadProps) {
  const [preview, setPreview] = useState<string | null>(previewUrl || null);

  const handleChange = (files: File[]) => {
    if (files.length > 0 && files[0].type.startsWith('image/')) {
      const url = URL.createObjectURL(files[0]);
      setPreview(url);
    }
    props.onChange?.(files);
  };

  return (
    <FileUpload
      {...props}
      accept={{ 'image/*': ['.png', '.jpg', '.jpeg', '.gif', '.webp'] }}
      onChange={handleChange}
      showFileList={false}
    >
      <div className="flex flex-col items-center justify-center p-8">
        {preview ? (
          <div className="relative">
            <img
              src={preview}
              alt="Preview"
              className="max-w-xs max-h-48 rounded-lg object-contain"
            />
            <div className="absolute top-2 right-2 bg-green-500 rounded-full p-1">
              <CheckCircle2 className="w-4 h-4 text-white" />
            </div>
          </div>
        ) : (
          <>
            <UploadCloud className="w-12 h-12 mb-3 text-muted-foreground" />
            <p className="text-muted-foreground mb-1">点击或拖拽上传图片</p>
            <p className="text-sm text-muted-foreground">
              支持 PNG, JPG, GIF, WebP
            </p>
          </>
        )}
      </div>
    </FileUpload>
  );
}
