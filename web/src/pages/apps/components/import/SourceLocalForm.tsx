import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import FileUpload from '@/components/file-upload';
import { Label } from '@/components/ui/label';
import { CheckCircle2, FileArchive, Package, X } from 'lucide-react';
import type { LocalMode } from '@/pages/apps/lib/importFlow';

/** What currently sits in the single local upload slot. */
export interface LocalUpload {
  /** .neoapp app package (server unpacks app.yaml + image.tar) or bare image tar. */
  kind: 'package' | 'image';
  fileName: string;
  /** Bytes as reported by the upload response (optional). */
  size?: number;
}

export interface SourceLocalFormProps {
  /** Current slot content (null = empty slot). */
  upload: LocalUpload | null;
  isUploading: boolean;
  progress: number;
  manifestMeta: { id: string; name?: string; version?: string } | null;
  imageTarName: string;
  existingAppIds: Set<string>;
  /** Which local mode the current upload resolves to (null = nothing yet). */
  localMode: LocalMode | null;
  /** Shell routes by extension: .neoapp → package unpack, tar → image only. */
  onUpload: (file: File) => void;
  /** Clears the slot (and, for a package, both halves it unpacked into). */
  onClear: () => void;
}

function formatFileSize(bytes?: number): string {
  if (!bytes || !Number.isFinite(bytes) || bytes <= 0) return '';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(2)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / 1024 / 1024).toFixed(2)} MB`;
  return `${(bytes / 1024 / 1024 / 1024).toFixed(2)} GB`;
}

/**
 * The expanded panel of the 本地上传 source card: ONE upload slot taking
 * either a .neoapp app package (server-side unpack fills manifest + image) or
 * a bare image tar (the form below generates the manifest). All upload,
 * cleanup and manifest-hydrate side effects live in the parent.
 */
export default function SourceLocalForm({
  upload,
  isUploading,
  progress,
  manifestMeta,
  imageTarName,
  existingAppIds,
  localMode,
  onUpload,
  onClear,
}: SourceLocalFormProps) {
  const { t } = useTranslation();

  const sizeText = formatFileSize(upload?.size);

  return (
    <div className="space-y-6">
      {localMode && (
        <div className="rounded-lg border border-border bg-muted/40 p-3 text-sm text-muted-foreground">
          {localMode === 'manifest'
            ? t(
                'sys.apps.import.local_mode_manifest_hint',
                '已上传 .neoapp 包：以包内 app.yaml 为准，可在下方微调'
              )
            : t(
                'sys.apps.import.local_mode_image_hint',
                '仅上传镜像：由下方表单生成配置文件'
              )}
        </div>
      )}

      <div>
        <Label className="text-base font-semibold mb-2 block">
          {t('sys.apps.import.local_upload_label', '上传应用包 / 镜像')}
        </Label>
        {upload ? (
          <>
            <div className="flex items-center justify-between gap-2 text-sm border rounded-lg p-3">
              <div className="flex items-center gap-2 min-w-0">
                <CheckCircle2
                  className={`w-5 h-5 shrink-0 ${
                    upload.kind === 'package'
                      ? 'text-green-500'
                      : 'text-muted-foreground'
                  }`}
                />
                <div className="min-w-0">
                  <div className="flex items-center gap-2 min-w-0">
                    {upload.kind === 'package' ? (
                      <FileArchive className="w-4 h-4 text-muted-foreground shrink-0" />
                    ) : (
                      <Package className="w-4 h-4 text-muted-foreground shrink-0" />
                    )}
                    <span className="font-medium text-foreground truncate">
                      {upload.fileName}
                    </span>
                    {sizeText && (
                      <span className="text-muted-foreground shrink-0">
                        · {sizeText}
                      </span>
                    )}
                  </div>
                  {upload.kind === 'package' ? (
                    <div className="text-muted-foreground truncate">
                      {t('sys.apps.import.package_extracted', '已解出')}
                      {manifestMeta?.id && ` ${manifestMeta.id}`}
                      {imageTarName && ` · ${imageTarName}`}
                    </div>
                  ) : (
                    <div className="text-muted-foreground truncate">
                      {imageTarName
                        || t('sys.apps.import.image_only_hint', '镜像')}
                    </div>
                  )}
                </div>
              </div>
              <Button variant="ghost" size="sm" onClick={onClear}>
                <X className="w-4 h-4" />
              </Button>
            </div>
            {upload.kind === 'package'
              && manifestMeta?.id
              && existingAppIds.has(manifestMeta.id) && (
                <p className="mt-1 text-sm text-red-500">
                  {t(
                    'sys.apps.import.duplicate_id_warning',
                    '此应用ID已存在，安装时将覆盖已有应用'
                  )}
                </p>
              )}
          </>
        ) : (
          <FileUpload
            single
            loading={isUploading}
            showProgress={isUploading}
            progress={progress}
            accept={{
              'application/gzip': ['.neoapp', '.tar.gz', '.tgz'],
              'application/x-gzip': ['.neoapp', '.tar.gz', '.tgz'],
              'application/x-tar': ['.tar'],
            }}
            maxSize={2 * 1024 * 1024 * 1024}
            placeholder={t(
              'sys.apps.import.local_upload_placeholder',
              '点击或拖入 .neoapp 应用包 / .tar 镜像'
            )}
            hint={t(
              'sys.apps.import.local_upload_hint',
              '.neoapp 自动解出配置与镜像；仅镜像时由表单生成配置。最大 2GB'
            )}
            onUpload={async files => {
              const file = files[0];
              if (file) onUpload(file);
            }}
          />
        )}
      </div>
    </div>
  );
}
