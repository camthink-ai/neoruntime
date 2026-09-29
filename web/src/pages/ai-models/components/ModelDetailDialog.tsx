import { useCallback, useEffect, useRef, useState } from 'react';
import { useTranslation } from 'react-i18next';
import {
  Dialog,
  DialogContent,
  DialogHeader,
  DialogTitle,
  DialogDescription,
} from '@/components/ui/dialog';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';
import { Badge } from '@/components/ui/badge';
import { Button } from '@/components/ui/button';
import { useModelInfo, useExportModel, useUpdateModel } from '@/hooks/useModels';
import { useToast } from '@/hooks/use-toast';
import {
  HardDrive,
  Clock,
  Tag,
  FolderOpen,
  ExternalLink,
  Hash,
  Settings2,
  AppWindow,
  Download,
  Power,
  PowerOff,
  Loader2,
  Pencil,
  type LucideIcon,
} from 'lucide-react';
import { getModelTypeLabel, getModelTypeDescription } from '../utils';
import { getModelIcon } from '../modelIcons';
import {
  apiErrorText,
  apiErrorCode,
  MODEL_LOAD_FAILED_CODE,
} from '../lib/apiErrors';
import {
  classifyOutputFormat,
  prefillUpdateForm,
  type ModelImportFormState,
} from '../lib/modelImportFlow';
import ModelConfigEditor, {
  useModelTypeOptions,
  type ModelConfigEditorHandle,
} from './import/ModelConfigEditor';
import TensorTable from './TensorTable';

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
  // Output delivery mode: 'platform' (plugin-decoded) | 'raw' (bare tensors)
  output_mode?: string;
  variant?: string;
  threshold?: number;
  max_detections?: number;
  file_size?: number;
  file_hash?: string;
  network_name?: string;
  used_by_apps?: string[];
  // Input dimensions from HEF
  input_width?: number;
  input_height?: number;
  // Output vstream names from the parse — classifies nms vs feature_map for
  // the edit mode's platform-decode guard.
  vstream_info?: string;
  // Schema-driven config; the API may send a parsed object or a JSON string
  config?: unknown;
}

interface ModelDetailDialogProps {
  model: ModelData | null;
  open: boolean;
  onOpenChange: (open: boolean) => void;
  /** runtime actions surfaced in the dialog footer (card/list pass theirs) */
  onLoad?: (modelId: string) => void;
  onUnload?: (modelId: string, modelName: string) => void;
  /** opens the import wizard in update mode to replace the model file */
  onUpdateFile?: (model: ModelData) => void;
  /** per-model busy predicate — index holds a Set so concurrent actions show. */
  isActionLoading?: (modelId: string) => boolean;
}

/** Chips shown before the "+N more" fold kicks in. */
const LABELS_FOLD_MAX = 8;

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

const hasNonEmptyString = (value: unknown): value is string => typeof value === 'string' && value.trim().length > 0;

const hasNumber = (value: unknown): value is number => typeof value === 'number' && !Number.isNaN(value);

/** Config arrives as a parsed object or a JSON string; normalize to a map. */
const parseModelConfig = (config: unknown): Record<string, unknown> | null => {
  if (!config) return null;
  if (typeof config === 'object' && !Array.isArray(config)) {
    return config as Record<string, unknown>;
  }
  if (typeof config === 'string' && config.trim().startsWith('{')) {
    try {
      const parsed: unknown = JSON.parse(config);
      if (
        typeof parsed === 'object'
        && parsed !== null
        && !Array.isArray(parsed)
      ) {
        return parsed as Record<string, unknown>;
      }
    } catch {
      return null;
    }
  }
  return null;
};

/** Labels live in config.labels as an array or comma-separated string. */
const extractLabels = (config: Record<string, unknown> | null): string[] => {
  const raw: unknown = config?.labels;
  if (Array.isArray(raw)) {
    return raw.map(item => String(item).trim()).filter(Boolean);
  }
  if (typeof raw === 'string') {
    return raw
      .split(/[,，\n]/)
      .map(item => item.trim())
      .filter(Boolean);
  }
  return [];
};

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
  onLoad,
  onUnload,
  onUpdateFile,
  isActionLoading,
}: ModelDetailDialogProps) {
  const { t } = useTranslation();
  const [labelsExpanded, setLabelsExpanded] = useState(false);
  const [unloadConfirmOpen, setUnloadConfirmOpen] = useState(false);

  // Inline edit mode: the shared configure editor over a prefilled form,
  // saved through the metadata-only update (the model id stays fixed).
  const [editing, setEditing] = useState(false);
  const [editForm, setEditForm] = useState<ModelImportFormState | null>(null);
  const [editConfirmOpen, setEditConfirmOpen] = useState(false);
  const initialEditFormRef = useRef<ModelImportFormState | null>(null);
  const editInitialProfileRef = useRef<string | null>(null);
  const editorRef = useRef<ModelConfigEditorHandle>(null);

  const modelTypeOptions = useModelTypeOptions();
  const modelId = open ? (model?.model_id ?? '') : '';
  const { data: modelDetail } = useModelInfo(modelId);
  const exportMutation = useExportModel();
  const updateMutation = useUpdateModel();
  const { toast } = useToast();

  // Leaving the dialog always drops any inline edit session with it.
  useEffect(() => {
    if (open) return;
    setEditing(false);
    setEditForm(null);
    initialEditFormRef.current = null;
    editInitialProfileRef.current = null;
    setEditConfirmOpen(false);
  }, [open]);

  const handleEditPatch = useCallback(
    (patch: Partial<ModelImportFormState>) => {
      setEditForm(prev => (prev ? { ...prev, ...patch } : prev));
    },
    []
  );

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
  const isLoaded = mergedModel.status === 'loaded';
  const isActing = isActionLoading?.(mergedModel.model_id) ?? false;

  // Only DB-backed device models (with a content hash) can be exported as
  // AMPK packages; runtime-only fallback rows have nothing to package.
  const canExport = hasNonEmptyString(mergedModel.file_hash);
  const isRawMode = mergedModel.output_mode === 'raw';

  const config = parseModelConfig(mergedModel.config);
  const labels = extractLabels(config);
  const nmsThreshold = config?.nms_threshold;
  const hasNmsThreshold = hasNumber(nmsThreshold);

  const hasPostprocessSection =    hasNonEmptyString(mergedModel.output_mode)
    || hasNumber(mergedModel.threshold)
    || hasNumber(mergedModel.max_detections)
    || hasNmsThreshold
    || labels.length > 0;

  const visibleLabels = labelsExpanded
    ? labels
    : labels.slice(0, LABELS_FOLD_MAX);
  const hiddenLabelsCount = labels.length - visibleLabels.length;

  const handleExport = () => {
    exportMutation.mutate(mergedModel.model_id, {
      onSuccess: () => {
        toast({
          title: t('sys.ai_models.detail.export_started', '导出已开始'),
          description: `${mergedModel.model_id}.bin`,
        });
      },
      onError: (error: any) => {
        toast({
          title: t('sys.ai_models.detail.export_failed', '导出失败'),
          description: error?.response?.data?.message || error?.message,
          variant: 'destructive',
        });
      },
    });
  };

  // Inline edit is offered only for types the capabilities schema knows —
  // the editor is schema-driven, so an unknown type has nothing to render.
  const editTypeOpt = modelTypeOptions.find(
    o => o.value === mergedModel.model_type
  );
  const canEdit = !!editTypeOpt;

  const isEditDirty = () => {
    if (!editing || !editForm) return false;
    const initial = initialEditFormRef.current;
    if (!initial) return true;
    return (
      editForm.modelId !== initial.modelId
      || editForm.modelType !== initial.modelType
      || editForm.outputMode !== initial.outputMode
      || editForm.variant !== initial.variant
      || JSON.stringify(editForm.config) !== JSON.stringify(initial.config)
    );
  };

  const exitEdit = () => {
    setEditing(false);
    setEditForm(null);
    initialEditFormRef.current = null;
    editInitialProfileRef.current = null;
    setEditConfirmOpen(false);
  };

  const handleStartEdit = () => {
    if (!editTypeOpt) return;
    // Spread into a plain snapshot: prefillUpdateForm reads promoted
    // top-level columns through its index signature.
    const prefilled = prefillUpdateForm(
      { ...mergedModel },
      editTypeOpt.fields
    );
    const rawProfile = prefilled.config.postprocess_profile;
    editInitialProfileRef.current =      typeof rawProfile === 'string' ? rawProfile : null;
    initialEditFormRef.current = prefilled;
    setEditForm(prefilled);
    setEditing(true);
  };

  const handleSave = () => {
    // Submit gate lives in the shared editor: touch everything, validate,
    // toast the first issue and jump to its section (null = do not send).
    const validForm = editorRef.current?.submit();
    if (!validForm) return;
    updateMutation.mutate(
      {
        // The id is fixed in edit mode — the editor renders it read-only.
        modelId: mergedModel.model_id,
        model_type: validForm.modelType,
        output_mode: validForm.outputMode,
        model_variant: validForm.variant.trim(),
        config: validForm.config,
      },
      {
        onSuccess: () => {
          toast({
            title: t(
              'sys.ai_models.message.update_success',
              '模型更新成功'
            ),
          });
          exitEdit();
        },
        onError: (error: any) => {
          // 5001 = the row committed but the NPU reload failed: report the
          // partial success as a warning and drop back to the view (the
          // refetched row shows the now-unloaded state).
          if (apiErrorCode(error) === MODEL_LOAD_FAILED_CODE) {
            toast({
              title: t(
                'sys.ai_models.message.update_reload_failed',
                '更新已保存，但在 NPU 上重新加载失败'
              ),
              description: apiErrorText(error),
              variant: 'warning',
            });
            exitEdit();
            return;
          }
          toast({
            title: t('common.error', '错误'),
            description: apiErrorText(error),
            variant: 'destructive',
          });
        },
      }
    );
  };

  // Esc / header X while editing intercepts the close: dirty work confirms
  // before being discarded, and both paths land back in view mode — the
  // dialog itself only ever closes from view mode.
  const handleDialogOpenChange = (nextOpen: boolean) => {
    if (nextOpen) {
      onOpenChange(true);
      return;
    }
    if (editing) {
      if (isEditDirty() && !updateMutation.isPending) {
        setEditConfirmOpen(true);
        return;
      }
      exitEdit();
      return;
    }
    onOpenChange(false);
  };

  // One labelled KV cell of a section grid.
  const item = (
    icon: LucideIcon,
    label: string,
    node: React.ReactNode,
    wide = false
  ) => {
    const Icon = icon;
    return (
      <div className={`min-w-0 space-y-1 ${wide ? 'col-span-2' : ''}`}>
        <div className="flex items-center gap-1.5 text-xs text-muted-foreground">
          <Icon className="w-3.5 h-3.5 shrink-0" />
          {label}
        </div>
        <div className="text-sm font-medium break-words">{node}</div>
      </div>
    );
  };

  // Grouped section wrapper — title row + two-column grid body.
  const section = (title: string, children: React.ReactNode) => (
    <section className="space-y-3">
      <h4 className="text-xs font-semibold uppercase tracking-wide text-muted-foreground border-b border-border pb-2">
        {title}
      </h4>
      <div className="grid grid-cols-1 sm:grid-cols-2 gap-4 min-w-0">
        {children}
      </div>
    </section>
  );

  return (
    <Dialog open={open} onOpenChange={handleDialogOpenChange}>
      <DialogContent
        className={`max-h-[90vh] flex flex-col overflow-hidden ${
          editing
            ? 'sm:max-w-4xl sm:h-[90vh] w-full max-w-[calc(100%-1rem)]'
            : 'sm:max-w-2xl'
        }`}
      >
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

        {editing && editForm ? (
          <div className="flex min-h-0 flex-1 flex-col">
            <ModelConfigEditor
              ref={editorRef}
              form={editForm}
              onPatch={handleEditPatch}
              isUpdate
              modelTypeOptions={modelTypeOptions}
              initialProfile={editInitialProfileRef.current}
              outputFormat={classifyOutputFormat(
                mergedModel.vstream_info ?? ''
              )}
              disabled={updateMutation.isPending}
            />
          </div>
        ) : (
        <div className="space-y-5 py-4 flex-1 min-h-0 overflow-y-auto">
          {/* Badges + type description */}
          <div className="space-y-2">
            <div className="flex items-center gap-2 flex-wrap">
              <Badge variant="secondary" className="rounded-full">
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
              {mergedModel.output_mode && (
                <Badge
                  variant={isRawMode ? 'outline' : 'secondary'}
                  className="rounded-full text-xs"
                >
                  {isRawMode
                    ? t('sys.ai_models.detail.output_mode_raw', '裸张量')
                    : t(
                        'sys.ai_models.detail.output_mode_platform',
                        '平台解码'
                      )}
                </Badge>
              )}
            </div>
            <p className="text-sm text-muted-foreground break-words">
              {typeInfo.description}
            </p>
          </div>

          {/* 基本信息 */}
          {section(
            t('sys.ai_models.detail.section_basic', '基本信息'),
            <>
              {item(
                Tag,
                t('sys.ai_models.detail.model_id', '模型 ID'),
                <span className="font-mono break-all">
                  {mergedModel.model_id}
                </span>,
                true
              )}
              {hasNonEmptyString(mergedModel.model_type)
                && item(
                  Tag,
                  t('sys.ai_models.detail.model_type', '模型类型'),
                  mergedModel.model_type
                )}
              {hasNonEmptyString(mergedModel.variant)
                && item(
                  Tag,
                  t('sys.ai_models.detail.variant', '变体'),
                  mergedModel.variant
                )}
              {hasNonEmptyString(mergedModel.version)
                && item(
                  Tag,
                  t('sys.ai_models.detail.version', '版本'),
                  mergedModel.version
                )}
              {inputSize
                && item(
                  ExternalLink,
                  t('sys.ai_models.detail.input_size', '输入尺寸'),
                  inputSize
                )}
              {hasNonEmptyString(mergedModel.network_name)
                && item(
                  Tag,
                  t('sys.ai_models.detail.network_name', '网络名称'),
                  mergedModel.network_name
                )}
            </>
          )}

          {/* 模型接口：输入/输出张量（无张量数据时整节隐藏） */}
          <TensorTable
            inputs={mergedModel.inputs}
            outputs={mergedModel.outputs}
          />

          {/* 运行状态 */}
          {section(
            t('sys.ai_models.detail.section_runtime', '运行状态'),
            <>
              {item(
                Clock,
                t('sys.ai_models.detail.status', '状态'),
                <Badge
                  variant={isLoaded ? 'default' : 'secondary'}
                  className={`text-xs ${
                    isLoaded ? 'bg-emerald-600 hover:bg-emerald-700' : ''
                  }`}
                >
                  {isLoaded
                    ? t('sys.ai_models.status.loaded', '已加载')
                    : t('sys.ai_models.status.uploaded', '未加载')}
                </Badge>
              )}
              {hasNumber(mergedModel.load_timestamp)
                && item(
                  Clock,
                  t('sys.ai_models.detail.load_time', '加载时间'),
                  formatTimestamp(mergedModel.load_timestamp)
                )}
              {hasNumber(mergedModel.estimated_tops)
                && item(
                  HardDrive,
                  t('sys.ai_models.detail.estimated_tops', '预估算力'),
                  `${mergedModel.estimated_tops}`
                )}
              {hasNumber(mergedModel.estimated_memory)
                && item(
                  HardDrive,
                  t('sys.ai_models.detail.estimated_memory', '预估内存'),
                  `${mergedModel.estimated_memory}`
                )}
            </>
          )}

          {/* 后处理参数 */}
          {hasPostprocessSection
            && section(
              t('sys.ai_models.detail.section_postprocess', '后处理参数'),
              <>
                {hasNonEmptyString(mergedModel.output_mode)
                  && item(
                    Settings2,
                    t('sys.ai_models.form.output_mode', '输出模式'),
                    isRawMode
                      ? t('sys.ai_models.detail.output_mode_raw', '裸张量')
                      : t(
                          'sys.ai_models.detail.output_mode_platform',
                          '平台解码'
                        )
                  )}
                {hasNumber(mergedModel.threshold)
                  && item(
                    Settings2,
                    t('sys.ai_models.detail.threshold', '置信阈值'),
                    `${(mergedModel.threshold * 100).toFixed(0)}%`
                  )}
                {hasNumber(mergedModel.max_detections)
                  && item(
                    Settings2,
                    t('sys.ai_models.detail.max_detections', '最大检测数'),
                    `${mergedModel.max_detections}`
                  )}
                {hasNmsThreshold
                  && item(
                    Settings2,
                    t('sys.ai_models.detail.nms_threshold', 'NMS 阈值'),
                    `${nmsThreshold}`
                  )}
                {labels.length > 0
                  && item(
                    Tag,
                    t('sys.ai_models.detail.labels', '类别标签'),
                    <div className="flex flex-wrap items-center gap-1.5">
                      {visibleLabels.map(label => (
                        <Badge
                          key={label}
                          variant="secondary"
                          className="text-xs font-mono"
                        >
                          {label}
                        </Badge>
                      ))}
                      {(hiddenLabelsCount > 0 || labelsExpanded) && (
                        <Button
                          type="button"
                          variant="ghost"
                          size="sm"
                          className="h-6 px-2 text-xs text-muted-foreground"
                          onClick={() => setLabelsExpanded(prev => !prev)}
                        >
                          {labelsExpanded
                            ? t('sys.ai_models.detail.labels_collapse', '收起')
                            : t(
                                'sys.ai_models.detail.labels_more',
                                '还有 {{count}} 个',
                                { count: hiddenLabelsCount }
                              )}
                        </Button>
                      )}
                    </div>,
                    true
                  )}
              </>
            )}

          {/* 关联与文件 */}
          {section(
            t('sys.ai_models.detail.section_files', '关联与文件'),
            <>
              {appsCount > 0
                && item(
                  AppWindow,
                  `${t('sys.ai_models.detail.used_by_apps', '关联应用')} (${appsCount})`,
                  <div className="flex flex-wrap gap-1.5">
                    {mergedModel.used_by_apps?.map((appId: string) => (
                      <Badge
                        key={appId}
                        variant="secondary"
                        className="text-xs"
                      >
                        {appId}
                      </Badge>
                    ))}
                  </div>,
                  true
                )}
              {hasNumber(mergedModel.file_size)
                && item(
                  HardDrive,
                  t('sys.ai_models.detail.file_size', '文件大小'),
                  formatFileSize(mergedModel.file_size)
                )}
              {hasNonEmptyString(mergedModel.model_path)
                && item(
                  FolderOpen,
                  t('sys.ai_models.detail.model_path', '模型路径'),
                  <code className="block w-full rounded-lg bg-muted/50 px-3 py-2 font-mono text-xs break-all whitespace-pre-wrap">
                    {mergedModel.model_path}
                  </code>,
                  true
                )}
              {hasNonEmptyString(mergedModel.file_hash)
                && item(
                  Hash,
                  t('sys.ai_models.detail.file_hash', '文件哈希'),
                  <code className="block w-full rounded-lg bg-muted/50 px-3 py-2 font-mono text-xs break-all whitespace-pre-wrap">
                    {mergedModel.file_hash}
                  </code>,
                  true
                )}
            </>
          )}
        </div>
        )}

        <div className="flex items-center justify-end gap-2 shrink-0">
          {editing ? (
            <>
              {onUpdateFile && (
                <Button
                  variant="outline"
                  disabled={updateMutation.isPending}
                  onClick={() => {
                    // The wizard is the full-featured path (file replace);
                    // drop the inline session on the way out.
                    exitEdit();
                    onUpdateFile(mergedModel);
                  }}
                >
                  {t('sys.ai_models.detail.action_replace_file', '更换文件')}
                </Button>
              )}
              <Button
                variant="outline"
                disabled={updateMutation.isPending}
                onClick={exitEdit}
              >
                {t('common.cancel', '取消')}
              </Button>
              <Button disabled={updateMutation.isPending} onClick={handleSave}>
                {updateMutation.isPending
                  ? t('sys.ai_models.detail.saving', '保存中...')
                  : t('sys.ai_models.detail.action_save', '保存')}
              </Button>
            </>
          ) : (
            <>
              {onLoad
                && onUnload
                && (isLoaded ? (
                  <Button
                    variant="outline"
                    disabled={isActing}
                    onClick={() => setUnloadConfirmOpen(true)}
                  >
                    {isActing ? (
                      <Loader2 className="w-4 h-4 mr-2 animate-spin" />
                    ) : (
                      <PowerOff className="w-4 h-4 mr-2" />
                    )}
                    {t('sys.ai_models.action.unload', '卸载')}
                  </Button>
                ) : (
                  <Button
                    disabled={isActing}
                    onClick={() => onLoad(mergedModel.model_id)}
                  >
                    {isActing ? (
                      <Loader2 className="w-4 h-4 mr-2 animate-spin" />
                    ) : (
                      <Power className="w-4 h-4 mr-2" />
                    )}
                    {t('sys.ai_models.action.load', '加载')}
                  </Button>
                ))}
              {canExport && (
                <Button
                  variant="outline"
                  onClick={handleExport}
                  disabled={exportMutation.isPending}
                >
                  <Download className="w-4 h-4 mr-2" />
                  {exportMutation.isPending
                    ? t('sys.ai_models.detail.exporting', '导出中...')
                    : t('sys.ai_models.detail.export_bin', '导出 .bin')}
                </Button>
              )}
              {canEdit && (
                <Button variant="outline" onClick={handleStartEdit}>
                  <Pencil className="w-4 h-4 mr-2" />
                  {t('sys.ai_models.detail.action_edit', '编辑')}
                </Button>
              )}
              <Button variant="outline" onClick={() => onOpenChange(false)}>
                {t('common.close', '关闭')}
              </Button>
            </>
          )}
        </div>
      </DialogContent>

      {/* Unload confirmation — mirrors the card/list dialogs */}
      <AlertDialog open={unloadConfirmOpen} onOpenChange={setUnloadConfirmOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.ai_models.confirm.unload_title', '确认卸载')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.ai_models.confirm.unload',
                '确认将模型 "{{name}}" 从 NPU 卸载？',
                { name: mergedModel.name || mergedModel.model_id }
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                onUnload?.(
                  mergedModel.model_id,
                  mergedModel.name || mergedModel.model_id
                );
                setUnloadConfirmOpen(false);
              }}
            >
              {t('sys.ai_models.action.unload', '卸载')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Edit discard confirmation — guards Esc / header X while editing.
          "Discard" returns to view mode; the dialog itself stays open. */}
      <AlertDialog open={editConfirmOpen} onOpenChange={setEditConfirmOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t(
                'sys.ai_models.wizard.close_confirm_title',
                '放弃未保存的修改？'
              )}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.ai_models.detail.edit_discard_desc',
                '未保存的修改将丢失。'
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>
              {t('sys.ai_models.wizard.close_confirm_keep', '继续编辑')}
            </AlertDialogCancel>
            <AlertDialogAction onClick={exitEdit}>
              {t('sys.ai_models.detail.edit_discard', '放弃修改')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </Dialog>
  );
}
