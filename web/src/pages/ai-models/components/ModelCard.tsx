import { useTranslation } from 'react-i18next';
import { useState } from 'react';
import { Link } from 'react-router-dom';
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
import {
  DropdownMenu,
  DropdownMenuTrigger,
  DropdownMenuContent,
  DropdownMenuItem,
} from '@/components/ui/dropdown-menu';
import {
  Eye,
  Trash2,
  Plus,
  Power,
  PowerOff,
  Loader2,
  Pencil,
  MoreHorizontal,
  AppWindow,
  SearchX,
} from 'lucide-react';
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
  /** provenance: "disk" = system preset, anything else = manually imported */
  source?: string;
}

interface ModelCardProps {
  models: ModelData[];
  /** total before filtering — distinguishes "no models" from "no match" */
  totalCount?: number;
  onClearFilters?: () => void;
  onDelete: (modelId: string, modelName: string) => void;
  onLoad: (modelId: string) => void;
  onUnload: (modelId: string, modelName: string) => void;
  onImportClick?: () => void;
  onUpdate?: (model: ModelData) => void;
  /** per-model busy predicate — index holds a Set so concurrent actions show. */
  isActionLoading?: (modelId: string) => boolean;
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
  totalCount,
  onClearFilters,
  onDelete,
  onLoad,
  onUnload,
  onImportClick,
  onUpdate,
  isActionLoading,
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

  const [updateConfirm, setUpdateConfirm] = useState<ModelData | null>(null);

  const isFilterNoMatch = models.length === 0 && (totalCount ?? 0) > 0;
  const isNoModels = (totalCount ?? models.length) === 0;

  return (
    <>
      {isFilterNoMatch && (
        <div className="mb-4 flex flex-col items-center gap-2 rounded-lg border border-dashed border-border bg-muted/20 px-6 py-8 text-center">
          <SearchX className="h-8 w-8 text-muted-foreground/60" />
          <p className="text-sm text-muted-foreground">
            {t('sys.ai_models.empty.no_match', '没有符合条件的模型')}
          </p>
          {onClearFilters && (
            <Button
              type="button"
              variant="outline"
              size="sm"
              onClick={onClearFilters}
            >
              {t('sys.ai_models.empty.clear_filters', '清除筛选')}
            </Button>
          )}
        </div>
      )}

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
          {isNoModels && (
            <p className="mt-3 max-w-[260px] text-xs text-muted-foreground/80 text-center">
              {t(
                'sys.ai_models.empty.intake_hint',
                '导入 .hef / .bin 模型包、扫描模型目录，或安装自带模型的应用，均可获得模型'
              )}
            </p>
          )}
        </div>

        {models.map(model => {
          const modelType = getModelType(model.model_type, model.model_id, t);
          const appsCount = model.used_by_apps?.length || 0;
          const isLoaded = model.status === 'loaded';
          const isLoading = isActionLoading?.(model.model_id) ?? false;

          return (
            <Card
              key={model.model_id}
              className="flex flex-col p-5 justify-between shadow-sm border-border hover:shadow-md transition-shadow cursor-pointer"
              onClick={() => setDetailModel(model)}
            >
              <div className="flex flex-col gap-3">
                {/* Header — icon + load status only; provenance/apps are
                    demoted to the meta line below the title. */}
                <div className="flex items-start justify-between gap-2">
                  <div className="w-10 h-10 rounded-lg flex items-center justify-center bg-primary/10 text-primary">
                    {getModelIcon(model.model_type, model.model_id, 'w-5 h-5')}
                  </div>
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
                </div>

                {/* Info */}
                <div>
                  <h3 className="font-semibold text-foreground leading-tight truncate">
                    {model.name || model.model_id}
                  </h3>
                  <p className="text-xs text-muted-foreground mt-1 truncate">
                    {modelType}
                    {model.variant && (
                      <span className="ml-1 opacity-70">({model.variant})</span>
                    )}
                  </p>
                  <p className="mt-1.5 flex items-center gap-1.5 text-xs text-muted-foreground/80">
                    <span>
                      {model.source === 'disk'
                        ? t('sys.ai_models.provenance.system')
                        : t('sys.ai_models.provenance.manual')}
                    </span>
                    {appsCount > 0 && (
                      <span className="flex items-center gap-0.5">
                        <AppWindow className="w-3 h-3" />
                        {appsCount}
                      </span>
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

              {/* Actions — the runtime toggle carries a text label (it is
                  the primary action); management entries collapse into the
                  ⋯ menu. Card click still opens the detail dialog. */}
              <div className="flex items-center justify-between gap-2 mt-4 pt-2 border-t">
                {isLoaded ? (
                  <Button
                    variant="outline"
                    size="sm"
                    className="h-8 border-orange-500/30 text-orange-600 hover:text-orange-500 dark:text-orange-400"
                    disabled={isLoading}
                    onClick={e => {
                      e.stopPropagation();
                      setUnloadConfirm({
                        id: model.model_id,
                        name: model.name || model.model_id,
                      });
                    }}
                  >
                    {isLoading ? (
                      <Loader2 className="w-4 h-4 mr-1.5 animate-spin" />
                    ) : (
                      <PowerOff className="w-4 h-4 mr-1.5" />
                    )}
                    {t('sys.ai_models.action.unload', '卸载')}
                  </Button>
                ) : (
                  <Button
                    variant="default"
                    size="sm"
                    className="h-8"
                    disabled={isLoading}
                    onClick={e => {
                      e.stopPropagation();
                      onLoad(model.model_id);
                    }}
                  >
                    {isLoading ? (
                      <Loader2 className="w-4 h-4 mr-1.5 animate-spin" />
                    ) : (
                      <Power className="w-4 h-4 mr-1.5" />
                    )}
                    {t('sys.ai_models.action.load', '加载')}
                  </Button>
                )}

                <DropdownMenu>
                  <DropdownMenuTrigger asChild>
                    <Button
                      variant="ghost"
                      size="icon"
                      className="h-8 w-8 text-muted-foreground hover:text-foreground"
                      aria-label={t('sys.ai_models.action.more', '更多操作')}
                      onClick={e => e.stopPropagation()}
                    >
                      <MoreHorizontal className="w-4 h-4" />
                    </Button>
                  </DropdownMenuTrigger>
                  {/* Portal or not, React synthetic events bubble through
                      the React tree: without this guard every menu item
                      click also fires the card's onClick and opens the
                      detail dialog behind the dialog the item opened. */}
                  <DropdownMenuContent
                    align="end"
                    className="w-40"
                    onClick={e => e.stopPropagation()}
                  >
                    <DropdownMenuItem onClick={() => setDetailModel(model)}>
                      <Eye className="w-4 h-4 mr-2" />
                      {t('common.detail', '详情')}
                    </DropdownMenuItem>
                    {onUpdate && (
                      <DropdownMenuItem
                        onClick={() => {
                          if (appsCount > 0) {
                            setUpdateConfirm(model);
                          } else {
                            onUpdate(model);
                          }
                        }}
                      >
                        <Pencil className="w-4 h-4 mr-2" />
                        {t('sys.ai_models.action.update', '更新')}
                      </DropdownMenuItem>
                    )}
                    <DropdownMenuItem
                      className="text-destructive focus:text-destructive"
                      onClick={() => {
                        setDeleteModel({
                          id: model.model_id,
                          name: model.name || model.model_id,
                          usedByApps: model.used_by_apps,
                        });
                      }}
                    >
                      <Trash2 className="w-4 h-4 mr-2" />
                      {t('common.delete', '删除')}
                    </DropdownMenuItem>
                  </DropdownMenuContent>
                </DropdownMenu>
              </div>
            </Card>
          );
        })}
      </div>

      <ModelDetailDialog
        model={detailModel}
        open={!!detailModel}
        onOpenChange={open => !open && setDetailModel(null)}
        onLoad={onLoad}
        onUnload={onUnload}
        onUpdateFile={onUpdate}
        isActionLoading={isActionLoading}
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
                      '该模型正在被以下应用引用（含未运行应用），请先删除引用关系后再删除模型：'
                    )}
                  </span>
                  <ul className="mt-2 space-y-1">
                    {deleteModel.usedByApps.map(app => (
                      <li
                        key={app}
                        className="font-medium text-foreground text-sm"
                      >
                        {/* Links jump to the apps page so the referencing
                            app can be located and removed without hunting
                            for its name by hand. */}
                        <Link
                          to="/apps"
                          className="underline decoration-border underline-offset-2 transition-colors hover:text-primary hover:decoration-primary"
                        >
                          {app}
                        </Link>
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

      {/* Update confirmation (model in use by apps) */}
      <AlertDialog
        open={!!updateConfirm}
        onOpenChange={open => !open && setUpdateConfirm(null)}
      >
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.ai_models.confirm.update_title', '确认更新')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {t(
                'sys.ai_models.confirm.update_in_use',
                '模型 "{{name}}" 正被 {{count}} 个应用使用，更新后相关应用可能受影响。确定继续？',
                {
                  name: updateConfirm?.name || updateConfirm?.model_id,
                  count: updateConfirm?.used_by_apps?.length ?? 0,
                }
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            <AlertDialogAction
              onClick={() => {
                if (updateConfirm) {
                  onUpdate?.(updateConfirm);
                  setUpdateConfirm(null);
                }
              }}
            >
              {t('sys.ai_models.action.update', '更新')}
            </AlertDialogAction>
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>
    </>
  );
}
