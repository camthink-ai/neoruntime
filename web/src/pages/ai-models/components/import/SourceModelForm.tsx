import { useTranslation } from 'react-i18next';
import { CheckCircle2, FileBox, Info, Package, X } from 'lucide-react';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import FileUpload from '@/components/file-upload';
import {
  formatFileSize,
  type ModelParseResult,
} from '../../lib/modelImportFlow';

export interface SourceModelFormProps {
  file: File | null;
  /** upload side effect (parse request) stays in the shell. */
  onFileChange: (files: File[]) => void;
  /** drop the picked file and its parse result, back to the empty slot. */
  onClear: () => void;
  isParsing: boolean;
  /** null = idle; 0-99 = uploading; 100 = body sent, server parsing. */
  uploadProgress: number | null;
  /** abort an in-flight upload/parse and return to the empty slot. */
  onCancelUpload: () => void;
  disabled: boolean;
  parseResult: ModelParseResult | null;
  /** '' | 'nms' | 'feature_map' — derived by the shell. */
  outputFormat: string;
  /** capabilities-derived accept map (mime → extensions), incl. .bin. */
  acceptFormats: Record<string, string[]>;
  formatHint: string;
  /** update mode: a file is optional — configuration-only update allowed. */
  isUpdate: boolean;
}

/**
 * Source screen of the model import wizard: upload slot + parsed-file card,
 * patterned after apps' SourceLocalForm. The card shows everything the
 * server sniffed (network, input size, suggested type, output format) plus
 * a mode hint — AMPK packages arrive pre-filled, bare HEFs need the form.
 */
export default function SourceModelForm({
  file,
  onFileChange,
  onClear,
  isParsing,
  uploadProgress,
  onCancelUpload,
  disabled,
  parseResult,
  outputFormat,
  acceptFormats,
  formatHint,
  isUpdate,
}: SourceModelFormProps) {
  const { t } = useTranslation();

  // 0-99 → bytes on the wire; 100/null while still pending → server-side
  // parse (indeterminate), which the bare "Parsing..." line covers.
  const isUploading = uploadProgress != null && uploadProgress < 100;

  return (
    <div className="space-y-4">
      {isUpdate && (
        <div className="rounded-lg border border-border bg-muted/40 p-3 text-sm text-muted-foreground">
          {t(
            'sys.ai_models.wizard.update_file_optional',
            'Optional: upload a new model file to replace the current one; skip to update configuration only'
          )}
        </div>
      )}

      {parseResult ? (
        <div className="space-y-3">
          <div className="flex items-start gap-3 rounded-lg border border-border bg-muted/20 p-4">
            <div className="mt-0.5 flex h-10 w-10 shrink-0 items-center justify-center rounded-lg bg-primary/10 text-primary">
              {parseResult.package ? (
                <Package className="h-5 w-5" />
              ) : (
                <FileBox className="h-5 w-5" />
              )}
            </div>
            <div className="min-w-0 flex-1 space-y-1.5">
              <div className="flex items-center gap-2">
                <span className="truncate text-sm font-medium text-foreground">
                  {parseResult.filename}
                </span>
                <span className="shrink-0 text-xs text-muted-foreground">
                  {formatFileSize(parseResult.file_size)}
                </span>
              </div>
              <div className="flex flex-wrap items-center gap-x-3 gap-y-1 text-xs text-muted-foreground">
                <span className="truncate">
                  {t('sys.ai_models.wizard.preview_network', 'Network')}:{' '}
                  {parseResult.network_name || '—'}
                </span>
                {parseResult.input_width && parseResult.input_height && (
                  <span
                    title={t(
                      'sys.ai_models.wizard.measured_input_hint',
                      'Measured from this compiled artifact; may differ from the network\'s nominal training resolution'
                    )}
                  >
                    {t('sys.ai_models.wizard.preview_input', 'Input')}:{' '}
                    {parseResult.input_width}×{parseResult.input_height}
                    <span className="ml-1 opacity-70">
                      {t(
                        'sys.ai_models.wizard.measured_input',
                        '(measured)'
                      )}
                    </span>
                  </span>
                )}
              </div>
              <div className="flex flex-wrap items-center gap-1.5 pt-0.5">
                <Badge variant="secondary" className="text-xs">
                  {t('sys.ai_models.wizard.suggested_type', 'Suggested')}:{' '}
                  {t(
                    `sys.ai_models.model_type.${parseResult.suggested_type}`,
                    parseResult.suggested_type
                  )}
                </Badge>
                {outputFormat && (
                  <Badge
                    variant={outputFormat === 'nms' ? 'default' : 'outline'}
                    className="text-xs"
                  >
                    {outputFormat === 'nms'
                      ? t(
                          'sys.ai_models.wizard.output_format_nms',
                          'NMS output'
                        )
                      : t(
                          'sys.ai_models.wizard.output_format_feature_map',
                          'Feature map output'
                        )}
                  </Badge>
                )}
                {parseResult.package && (
                  <Badge variant="outline" className="text-xs">
                    AMPK
                  </Badge>
                )}
              </div>
            </div>
            <div className="flex shrink-0 flex-col items-end gap-2">
              <CheckCircle2 className="h-4 w-4 text-green-500" />
              <Button
                type="button"
                variant="ghost"
                size="sm"
                onClick={onClear}
                disabled={disabled}
                className="h-7 px-2 text-xs text-muted-foreground hover:text-foreground"
              >
                <X className="mr-1 h-3.5 w-3.5" />
                {t('common.clear', 'Clear')}
              </Button>
            </div>
          </div>

          <div
            className={`flex items-start gap-2 rounded-lg border p-3 text-sm ${
              parseResult.package
                ? 'border-blue-500/60 bg-blue-500/10 text-blue-700 dark:text-blue-400'
                : 'border-border bg-muted/40 text-muted-foreground'
            }`}
          >
            {parseResult.package ? (
              <Info className="mt-0.5 h-4 w-4 shrink-0" />
            ) : (
              <FileBox className="mt-0.5 h-4 w-4 shrink-0" />
            )}
            <span>
              {parseResult.package
                ? t(
                    'sys.ai_models.wizard.mode_package_hint',
                    'AMPK package detected — configuration is pre-filled from the package metadata; review and adjust on the next step'
                  )
                : t(
                    'sys.ai_models.wizard.mode_hef_hint',
                    'Bare HEF file — complete the model configuration on the next step'
                  )}
            </span>
          </div>
        </div>
      ) : (
        <div className="space-y-3">
          <FileUpload
            single
            value={file ? [file] : []}
            onChange={onFileChange}
            loading={isParsing}
            disabled={disabled}
            showFileList
            showProgress={isUploading}
            progress={uploadProgress ?? 0}
            accept={acceptFormats}
            placeholder={t(
              'sys.ai_models.form.file_placeholder',
              'Drag and drop model file here'
            )}
            hint={formatHint}
          />
          {isParsing && (
            <div className="flex items-center gap-3">
              <p className="flex-1 animate-pulse text-sm text-muted-foreground">
                {isUploading
                  ? t('sys.ai_models.wizard.uploading', 'Uploading...')
                  : t('sys.ai_models.wizard.parsing', 'Parsing model...')}
              </p>
              <Button
                type="button"
                variant="outline"
                size="sm"
                onClick={onCancelUpload}
              >
                {t('common.cancel', 'Cancel')}
              </Button>
            </div>
          )}
        </div>
      )}
    </div>
  );
}
