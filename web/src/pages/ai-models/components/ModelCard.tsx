import { useTranslation } from 'react-i18next';
import { Card } from '@/components/ui/card';
import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
import {
  AlertDialog,
  AlertDialogContent,
  AlertDialogHeader,
  AlertDialogTitle,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogCancel,
  AlertDialogAction,
} from '@/components/ui/alert-dialog';
import { Eye, Trash2, Plus, Power, PowerOff, Loader2 } from 'lucide-react';
import { useState } from 'react';
import ModelDetailDialog from './ModelDetailDialog';
import { resolveModelType } from '../utils';
import { getModelIcon } from '../modelIcons';

interface ModelData {
  model_id: string;
  name?: string;
  model_path?: string;
  load_timestamp?: number;
  status?: string;
  model_type?: string;
  variant?: string;
  threshold?: number;
  max_detections?: number;
  file_size?: number;
  used_by_apps?: string[];
  input_width?: number;
  input_height?: number;
}

interface ModelCardProps {
  models: ModelData[];
  onDelete: (modelId: string, modelName: string) => void;
  onLoad: (modelId: string) => void;
  onUnload: (modelId: string, modelName: string) => void;
  onImportClick?: () => void;
  loadingAction?: string | null;
}

const getModelType = (
  modelType: string | undefined,
  modelId: string,
  t: any
): string => {
  const key = resolveModelType(modelType, modelId);
  if (key) return t(`sys.ai_models.model_type.${key}`);
  if (modelType) return modelType;
  return t('sys.ai_models.type.ai', 'AI Model');
};

const formatFileSize = (bytes: number | undefined): string => {
  if (!bytes) return '-';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
};

const formatPath = (path: string | undefined): string => {
  if (!path) return '-';
  const parts = path.split('/');
  return parts.length > 2 ? `.../${parts.slice(-2).join('/')}` : path;
};

export default function ModelCard({
  models,
  onDelete,
  onLoad,
  onUnload,
  onImportClick,
  loadingAction,
}: ModelCardProps) {
  const { t } = useTranslation();
  const [detailModel, setDetailModel] = useState<ModelData | null>(null);
  const [deleteModel, setDeleteModel] = useState<{
    id: string;
    name: string;
    usedByApps?: string[];
  } | null>(null);

  const [unloadConfirm, setUnloadConfirm] = useState<{
    id: string;
    name: string;
  } | null>(null);

  if (models.length === 0) {
    return (
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4">
        <div
          className="flex flex-col items-center justify-center p-6 border-2 border-dashed border-border rounded-lg hover:border-primary hover:bg-primary/5 transition-all cursor-pointer min-h-[220px]"
          onClick={() => onImportClick?.()}
        >
          <div className="w-16 h-16 rounded-full bg-muted flex items-center justify-center mb-4">
            <Plus className="w-8 h-8 text-muted-foreground" />
          </div>
          <h3 className="text-lg font-semibold text-foreground mb-2">
            {t('sys.ai_models.action.import', '导入模型')}
          </h3>
          <p className="text-sm text-muted-foreground text-center">
            {t('sys.ai_models.action.import_desc')}
          </p>
        </div>
      </div>
    );
  }

  return (
    <>
      <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4">
        {/* Import card */}
        <div
          className="flex flex-col items-center justify-center p-6 border-2 border-dashed border-border rounded-lg hover:border-primary hover:bg-primary/5 transition-all cursor-pointer min-h-[220px]"
          onClick={() => onImportClick?.()}
        >
          <div className="w-16 h-16 rounded-full bg-muted flex items-center justify-center mb-4">
            <Plus className="w-8 h-8 text-muted-foreground" />
          </div>
          <h3 className="text-lg font-semibold text-foreground mb-2">
            {t('sys.ai_models.action.import', '导入模型')}
          </h3>
          <p className="text-sm text-muted-foreground text-center">
            {t('sys.ai_models.action.import_desc')}
          </p>
        </div>

        {models.map(model => {
          const modelType = getModelType(model.model_type, model.model_id, t);
          const appsCount = model.used_by_apps?.length || 0;
          const isLoaded = model.status === 'loaded';
          const isLoading = loadingAction === model.model_id;

          return (
            <Card
              key={model.model_id}
              className="flex flex-col p-5 justify-between shadow-sm border-border hover:shadow-md transition-shadow cursor-pointer"
              onClick={() => setDetailModel(model)}
            >
              <div className="flex flex-col gap-3">
                {/* Header */}
                <div className="flex items-start justify-between">
                  <div className="w-10 h-10 rounded-lg flex items-center justify-center bg-primary/10 text-primary">
                    {getModelIcon(model.model_type, model.model_id, 'w-5 h-5')}
                  </div>
                  <div className="flex items-center gap-2">
                    <Badge
                      variant={isLoaded ? 'default' : 'secondary'}
                      className={`text-xs ${isLoaded ? 'bg-emerald-600 hover:bg-emerald-700' : ''}`}
                    >
                      {isLoaded
                        ? t('sys.ai_models.status.loaded', '已加载')
                        : t('sys.ai_models.status.uploaded', '未加载')}
                    </Badge>
                    {appsCount > 0 && (
                      <Badge variant="secondary" className="text-xs">
                        {appsCount} {t('sys.ai_models.apps', '应用')}
                      </Badge>
                    )}
                  </div>
                </div>

                {/* Info */}
                <div>
                  <h3 className="font-semibold text-foreground leading-tight truncate">
                    {model.name || model.model_id}
                  </h3>
                  <p className="text-xs text-muted-foreground mt-1">
                    {modelType}
                    {model.variant && (
                      <span className="ml-1 opacity-70">({model.variant})</span>
                    )}
                  </p>
                </div>

                {/* Details */}
                <div className="space-y-1.5 text-xs text-muted-foreground">
                  <div className="flex items-center justify-between">
                    <span>ID</span>
                    <span
                      className="font-mono truncate max-w-[150px]"
                      title={model.model_id}
                    >
                      {model.model_id}
                    </span>
                  </div>
                  <div className="flex items-center justify-between">
                    <span>
                      {t('sys.ai_models.detail.input_size', '输入尺寸')}
                    </span>
                    <span>
                      {model.input_width && model.input_height
                        ? `${model.input_width}×${model.input_height}`
                        : '-'}
                    </span>
                  </div>
                  <div className="flex items-center justify-between">
                    <span>
                      {t('sys.ai_models.detail.file_size', '文件大小')}
                    </span>
                    <span>{formatFileSize(model.file_size)}</span>
                  </div>
                  <div className="flex items-center justify-between">
                    <span>{t('sys.ai_models.table.model_path', '路径')}</span>
                    <span
                      className="font-mono truncate max-w-[150px]"
                      title={model.model_path}
                    >
                      {formatPath(model.model_path)}
                    </span>
                  </div>
                </div>
              </div>

              {/* Actions */}
              <div className="flex items-center justify-end gap-1 mt-4 pt-2 border-t">
                {isLoaded ? (
                  <Button
                    variant="ghost"
                    size="sm"
                    className="h-8 px-3 text-muted-foreground hover:text-orange-500"
                    disabled={isLoading}
                    title={t('sys.ai_models.action.unload', '卸载')}
                    onClick={e => {
                      e.stopPropagation();
                      setUnloadConfirm({
                        id: model.model_id,
                        name: model.name || model.model_id,
                      });
                    }}
                  >
                    {isLoading ? (
                      <Loader2 className="w-4 h-4 animate-spin" />
                    ) : (
                      <PowerOff className="w-4 h-4" />
                    )}
                  </Button>
                ) : (
                  <Button
                    variant="ghost"
                    size="sm"
                    className="h-8 px-3 text-muted-foreground hover:text-emerald-500"
                    disabled={isLoading}
                    title={t('sys.ai_models.action.load', '加载')}
                    onClick={e => {
                      e.stopPropagation();
                      onLoad(model.model_id);
                    }}
                  >
                    {isLoading ? (
                      <Loader2 className="w-4 h-4 animate-spin" />
                    ) : (
                      <Power className="w-4 h-4" />
                    )}
                  </Button>
                )}
                <Button
                  variant="ghost"
                  size="sm"
                  className="h-8 px-3 text-muted-foreground hover:text-foreground"
                  title={t('common.detail', '详情')}
                  onClick={e => {
                    e.stopPropagation();
                    setDetailModel(model);
                  }}
                >
                  <Eye className="w-4 h-4" />
                </Button>
                <Button
                  variant="ghost"
                  size="sm"
                  className="h-8 px-3 text-muted-foreground hover:text-destructive"
                  title={t('common.delete', '删除')}
                  onClick={e => {
                    e.stopPropagation();
                    setDeleteModel({
                      id: model.model_id,
                      name: model.name || model.model_id,
                      usedByApps: model.used_by_apps,
                    });
                  }}
                >
                  <Trash2 className="w-4 h-4" />
                </Button>
              </div>
            </Card>
          );
        })}
      </div>

      <ModelDetailDialog
        model={detailModel}
        open={!!detailModel}
        onOpenChange={open => !open && setDetailModel(null)}
      />

      {/* Delete confirmation */}
      <AlertDialog
        open={!!deleteModel}
        onOpenChange={open => !open && setDeleteModel(null)}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('common.delete_confirm_title', '确认删除')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {deleteModel?.usedByApps && deleteModel.usedByApps.length > 0 ? (
                <>
                  <span>
                    {t(
                      'sys.ai_models.message.delete_blocked',
                      '该模型正在被以下应用引用，请先删除引用关系后再删除模型：'
                    )}
                  </span>
                  <ul className="mt-2 space-y-1">
                    {deleteModel.usedByApps.map(app => (
                      <li
                        key={app}
                        className="font-medium text-foreground text-sm"
                      >
                        • {app}
                      </li>
                    ))}
                  </ul>
                </>
              ) : (
                <>
                  {t(
                    'common.delete_confirm_description',
                    '确定要删除此模型吗？此操作无法撤销。'
                  )}
                  {deleteModel && (
                    <span className="block mt-2 font-medium text-foreground">
                      {deleteModel.name}
                    </span>
                  )}
                </>
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            {(!deleteModel?.usedByApps
              || deleteModel.usedByApps.length === 0) && (
              <AlertDialogAction
                className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
                onClick={() => {
                  if (deleteModel) {
                    onDelete(deleteModel.id, deleteModel.name);
                    setDeleteModel(null);
                  }
                }}
              >
                {t('common.confirm', '确认')}
              </AlertDialogAction>
            )}
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Unload confirmation */}
      <AlertDialog
        open={!!unloadConfirm}
        onOpenChange={open => !open && setUnloadConfirm(null)}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.ai_models.confirm.unload_title', '确认卸载')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.ai_models.confirm.unload',
                '确认将模型 "{{name}}" 从 NPU 卸载？',
                { name: unloadConfirm?.name }
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                if (unloadConfirm) {
                  onUnload(unloadConfirm.id, unloadConfirm.name);
                  setUnloadConfirm(null);
                }
              }}
            >
              {t('sys.ai_models.action.unload', '卸载')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </>
  );
}
