import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
} from '@/components/ui/dialog';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { useModelInfo } from '@/hooks/useModels';
import {
  HardDrive,
  Clock,
  Tag,
  FolderOpen,
  ExternalLink,
  Settings2,
  AppWindow,
} from 'lucide-react';
import { getModelTypeLabel, getModelTypeDescription } from '../utils';
import { getModelIcon } from '../modelIcons';

interface ModelData {
  model_id: string;
  name?: string;
  model_path?: string;
  version?: string;
  load_timestamp?: number;
  status?: string;
  estimated_memory?: number;
  estimated_tops?: number;
  inputs?: unknown;
  outputs?: unknown;
  // Enriched fields from database
  model_type?: string;
  variant?: string;
  threshold?: number;
  max_detections?: number;
  file_size?: number;
  used_by_apps?: string[];
  // Input dimensions from HEF
  input_width?: number;
  input_height?: number;
}

interface ModelDetailDialogProps {
  model: ModelData | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
}

// Format timestamp to readable time
const formatTimestamp = (timestamp: number | undefined): string => {
  if (!timestamp) return '-';
  const date = new Date(timestamp * 1000);
  return date.toLocaleString();
};

// Format file size
const formatFileSize = (bytes: number | undefined): string => {
  if (!bytes) return '-';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
};

const formatIoSummary = (io: unknown): string => {
  if (!io) return '-';
  if (Array.isArray(io)) return `${io.length}`;
  if (typeof io === 'object') return `${Object.keys(io as Record<string, unknown>).length}`;
  return '-';
};

const hasNonEmptyString = (value: unknown): value is string => typeof value === 'string' && value.trim().length > 0;

const hasNumber = (value: unknown): value is number => typeof value === 'number' && !Number.isNaN(value);

// Get model type info using shared utilities
const getModelTypeInfo = (
  modelType: string | undefined,
  modelId: string,
  t: any
): { type: string; description: string } => ({
  type: getModelTypeLabel(modelType, modelId, t),
  description: getModelTypeDescription(modelType, modelId, t),
});

export default function ModelDetailDialog({
  model,
  open,
  onOpenChange,
}: ModelDetailDialogProps) {
  const { t } = useTranslation();

  const modelId = open ? (model?.model_id ?? '') : '';
  const { data: modelDetail } = useModelInfo(modelId);

  if (!model) return null;
  const mergedModel: ModelData =    modelDetail && typeof modelDetail === 'object'
      ? { ...model, ...(modelDetail as Partial<ModelData>) }
      : model;

  const typeInfo = getModelTypeInfo(
    mergedModel.model_type,
    mergedModel.model_id,
    t
  );
  const inputSize =    hasNumber(mergedModel.input_width) && hasNumber(mergedModel.input_height)
      ? `${mergedModel.input_width} × ${mergedModel.input_height}`
      : null;
  const appsCount = mergedModel.used_by_apps?.length || 0;

  return (
    <Dialog open={open} onOpenChange={onOpenChange}>
      <DialogContent className="sm:max-w-lg max-h-[90vh] flex flex-col overflow-hidden">
        <DialogHeader className="min-w-0 shrink-0">
          <DialogTitle className="flex items-start gap-2 min-w-0">
            <div className="w-8 h-8 shrink-0 rounded-lg flex items-center justify-center bg-primary/10 text-primary">
              {getModelIcon(
                mergedModel.model_type,
                mergedModel.model_id,
                'w-4 h-4'
              )}
            </div>
            <span className="min-w-0 flex-1 break-all leading-snug">
              {mergedModel.name || mergedModel.model_id}
            </span>
          </DialogTitle>
          <DialogDescription className="sr-only">
            {t('sys.ai_models.detail.description', '模型详情')}
          </DialogDescription>
        </DialogHeader>

        <div className="space-y-6 py-4 flex-1 min-h-0 overflow-y-auto">
          {/* Status Badge */}
          <div className="flex items-center gap-3 flex-wrap">
            <Badge
              variant="secondary"
              className="max-w-full rounded-full break-words whitespace-normal"
            >
              {typeInfo.type}
            </Badge>
            {mergedModel.variant && (
              <Badge
                variant="outline"
                className="max-w-full rounded-full text-xs break-words whitespace-normal"
              >
                {mergedModel.variant}
              </Badge>
            )}
          </div>

          {/* Description */}
          <p className="text-sm text-muted-foreground break-words">
            {typeInfo.description}
          </p>

          {/* Details Grid */}
          <div className="grid grid-cols-2 gap-4 min-w-0">
            {/* Model ID */}
            <div className="min-w-0 space-y-1">
              <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                <Tag className="w-3.5 h-3.5 shrink-0" />
                {t('sys.ai_models.detail.model_id', '模型 ID')}
              </div>
              <div className="text-sm font-mono font-medium break-all">
                {mergedModel.model_id}
              </div>
            </div>

            {hasNonEmptyString(mergedModel.model_type) && (
              <div className="min-w-0 space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  {getModelIcon(
                    mergedModel.model_type,
                    mergedModel.model_id,
                    'w-3.5 h-3.5'
                  )}
                  {t('sys.ai_models.detail.model_type', '模型类型')}
                </div>
                <div className="text-sm font-medium break-words">
                  {mergedModel.model_type}
                </div>
              </div>
            )}

            {hasNonEmptyString(mergedModel.version) && (
              <div className="min-w-0 space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <Tag className="w-3.5 h-3.5 shrink-0" />
                  {t('sys.ai_models.detail.version', '版本')}
                </div>
                <div className="text-sm font-medium break-words">
                  {mergedModel.version}
                </div>
              </div>
            )}

            {inputSize && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <ExternalLink className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.input_size', '输入尺寸')}
                </div>
                <div className="text-sm font-medium">{inputSize}</div>
              </div>
            )}

            {/* Load Time */}
            {hasNumber(mergedModel.load_timestamp) && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <Clock className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.load_time', '加载时间')}
                </div>
                <div className="text-sm font-medium">
                  {formatTimestamp(mergedModel.load_timestamp)}
                </div>
              </div>
            )}

            {/* File Size */}
            {hasNumber(mergedModel.file_size) && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <HardDrive className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.file_size', '文件大小')}
                </div>
                <div className="text-sm font-medium">
                  {formatFileSize(mergedModel.file_size)}
                </div>
              </div>
            )}

            {/* Threshold */}
            {hasNumber(mergedModel.threshold) && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <Settings2 className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.threshold', '置信阈值')}
                </div>
                <div className="text-sm font-medium">
                  {`${(mergedModel.threshold * 100).toFixed(0)}%`}
                </div>
              </div>
            )}

            {/* Estimated TOPS */}
            {hasNumber(mergedModel.estimated_tops) && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <HardDrive className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.estimated_tops', '预估算力')}
                </div>
                <div className="text-sm font-medium">{`${mergedModel.estimated_tops}`}</div>
              </div>
            )}

            {/* Estimated Memory */}
            {hasNumber(mergedModel.estimated_memory) && (
              <div className="space-y-1">
                <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                  <HardDrive className="w-3.5 h-3.5" />
                  {t('sys.ai_models.detail.estimated_memory', '预估内存')}
                </div>
                <div className="text-sm font-medium">{`${mergedModel.estimated_memory}`}</div>
              </div>
            )}

            {/* Inputs */}
            {mergedModel.inputs !== null
              && mergedModel.inputs !== undefined && (
                <div className="space-y-1">
                  <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                    <ExternalLink className="w-3.5 h-3.5" />
                    {t('sys.ai_models.detail.inputs', '输入')}
                  </div>
                  <div className="text-sm font-medium">
                    {formatIoSummary(mergedModel.inputs)}
                  </div>
                </div>
              )}

            {/* Outputs */}
            {mergedModel.outputs !== null
              && mergedModel.outputs !== undefined && (
                <div className="space-y-1">
                  <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                    <ExternalLink className="w-3.5 h-3.5" />
                    {t('sys.ai_models.detail.outputs', '输出')}
                  </div>
                  <div className="text-sm font-medium">
                    {formatIoSummary(mergedModel.outputs)}
                  </div>
                </div>
              )}
          </div>

          {/* Model Path */}
          {hasNonEmptyString(mergedModel.model_path) && (
            <div className="min-w-0 space-y-1.5">
              <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                <FolderOpen className="w-3.5 h-3.5 shrink-0" />
                {t('sys.ai_models.detail.model_path', '模型路径')}
              </div>
              <div className="min-w-0 overflow-hidden bg-muted/50 rounded-lg px-3 py-2">
                <code className="block w-full text-xs font-mono break-all whitespace-pre-wrap">
                  {mergedModel.model_path}
                </code>
              </div>
            </div>
          )}

          {/* Associated Apps */}
          {appsCount > 0 && (
            <div className="space-y-2">
              <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
                <AppWindow className="w-3.5 h-3.5" />
                {t('sys.ai_models.detail.used_by_apps', '关联应用')} (
                {appsCount})
              </div>
              <div className="flex flex-wrap gap-1.5">
                {mergedModel.used_by_apps?.map((appId: string) => (
                  <Badge key={appId} variant="secondary" className="text-xs">
                    {appId}
                  </Badge>
                ))}
              </div>
            </div>
          )}

          {/* Hardware Info */}
          {/* <div className="border-t pt-4">
            <div className="flex items-center gap-2 text-sm text-muted-foreground">
              <HardDrive className="w-4 h-4" />
              <span>
                {t('sys.ai_models.detail.hardware', '运行设备')}: Hailo-8 NPU (26 TOPS)
              </span>
            </div>
          </div> */}
        </div>

        <div className="flex justify-end gap-2 shrink-0">
          <Button variant="outline" onClick={() => onOpenChange(false)}>
            {t('common.close', '关闭')}
          </Button>
        </div>
      </DialogContent>
    </Dialog>
  );
}
