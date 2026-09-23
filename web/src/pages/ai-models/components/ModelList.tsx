import { useTranslation } from 'react-i18next';
import { Link } from 'react-router-dom';

import { Button } from '@/components/ui/button';
import { Badge } from '@/components/ui/badge';
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
import {
  ChevronLeft,
  ChevronRight,
  Eye,
  Trash2,
  AppWindow,
  Power,
  PowerOff,
  Loader2,
  Pencil,
} from 'lucide-react';

import { Empty } from '@/components/ui/empty';
import { TruncateWithTooltip } from '@/components/truncate-with-tooltip';
import { useState } from 'react';
import ModelDetailDialog from './ModelDetailDialog';
import { getModelTypeLabel } from '../utils';

interface ModelData {
  model_id: string;
  name?: string;
  model_path?: string;
  version?: string;
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

interface ModelListProps {
  models: ModelData[];
  /** total before filtering — distinguishes "no models" from "no match" */
  totalCount?: number;
  onClearFilters?: () => void;
  onDelete: (modelId: string, modelName: string) => void;
  onLoad: (modelId: string) => void;
  onUnload: (modelId: string, modelName: string) => void;
  onUpdate?: (model: ModelData) => void;
  /** per-model busy predicate — index holds a Set so concurrent actions show. */
  isActionLoading?: (modelId: string) => boolean;
}

const PAGE_SIZE = 10;

const formatLoadTime = (timestamp: number | undefined, t: any): string => {
  if (!timestamp) return '-';
  const now = Date.now() / 1000;
  const diff = now - timestamp;
  if (diff < 60) return t('sys.ai_models.time.just_now', '刚刚');
  if (diff < 3600) return `${Math.floor(diff / 60)} ${t('sys.ai_models.time.minutes_ago', '分钟前')}`;
  if (diff < 86400) return `${Math.floor(diff / 3600)} ${t('sys.ai_models.time.hours_ago', '小时前')}`;
  return `${Math.floor(diff / 86400)} ${t('sys.ai_models.time.days_ago', '天前')}`;
};

const formatFileSize = (bytes: number | undefined): string => {
  if (!bytes) return '-';
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
};

export default function ModelList({
  models,
  totalCount,
  onClearFilters,
  onDelete,
  onLoad,
  onUnload,
  onUpdate,
  isActionLoading,
}: ModelListProps) {
  const { t } = useTranslation();
  const [currentPage, setCurrentPage] = useState(1);
  const [deleteDialogOpen, setDeleteDialogOpen] = useState(false);
  const [modelToDelete, setModelToDelete] = useState<{
    id: string;
    name: string;
    usedByApps?: string[];
  } | null>(null);
  const [detailModel, setDetailModel] = useState<ModelData | null>(null);
  const [unloadConfirm, setUnloadConfirm] = useState<{
    id: string;
    name: string;
  } | null>(null);
  const [updateConfirm, setUpdateConfirm] = useState<ModelData | null>(null);

  const totalPages = Math.max(1, Math.ceil(models.length / PAGE_SIZE));
  const safePage = Math.min(currentPage, totalPages);
  const startIndex = (safePage - 1) * PAGE_SIZE;
  const pagedModels = models.slice(startIndex, startIndex + PAGE_SIZE);

  const handleDeleteClick = (model: ModelData) => {
    setModelToDelete({
      id: model.model_id,
      name: model.name || model.model_id,
      usedByApps: model.used_by_apps,
    });
    setDeleteDialogOpen(true);
  };

  const confirmDelete = () => {
    if (modelToDelete) {
      onDelete(modelToDelete.id, modelToDelete.name);
      setDeleteDialogOpen(false);
      setModelToDelete(null);
    }
  };

  return (
    <>
      <div className="bg-card rounded-xl border border-border shadow-sm overflow-hidden">
        <table className="w-full min-w-[1100px] text-sm text-left">
          <thead className="border-b border-border bg-background text-muted-foreground font-medium shadow-sm">
            <tr>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.table.model_id', 'Model ID')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.table.type', '类型')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.table.status', '状态')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.detail.input_size', '输入尺寸')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.detail.file_size', '文件大小')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.table.model_path', '路径')}
              </th>
              <th className="px-6 py-4 font-medium">
                {t('sys.ai_models.table.load_time', '加载时间')}
              </th>
              <th className="sticky right-0 top-0 z-10 bg-background px-6 py-4 font-medium shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.1)] dark:shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.3)]">
                {t('sys.ai_models.table.actions', '操作')}
              </th>
            </tr>
          </thead>
          <tbody className="divide-y divide-border">
            {pagedModels.length === 0 ? (
              <tr>
                <td
                  colSpan={8}
                  className="px-6 py-12 text-center text-muted-foreground"
                >
                  {models.length === 0 && (totalCount ?? 0) > 0 ? (
                    <div className="flex flex-col items-center gap-3">
                      <Empty
                        description={t(
                          'sys.ai_models.empty.no_match',
                          '没有符合条件的模型'
                        )}
                      />
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
                  ) : (
                    <Empty
                      description={t(
                        'sys.ai_models.empty.installed',
                        '暂无已安装的AI模型'
                      )}
                    />
                  )}
                </td>
              </tr>
            ) : (
              pagedModels.map(model => {
                const isLoaded = model.status === 'loaded';
                const isLoading = isActionLoading?.(model.model_id) ?? false;
                const modelType = getModelTypeLabel(
                  model.model_type,
                  model.model_id,
                  t
                );
                const appsCount = model.used_by_apps?.length || 0;

                return (
                  <tr
                    key={model.model_id}
                    className="group hover:bg-muted/50 transition-colors"
                  >
                    <td className="px-6 py-4">
                      <div className="flex items-center gap-3">
                        <div
                          className={`w-2 h-2 rounded-full ${
                            isLoaded
                              ? 'bg-emerald-500'
                              : 'bg-muted-foreground/40'
                          }`}
                        />
                        <div>
                          <div className="font-semibold text-foreground">
                            {model.name || model.model_id}
                          </div>
                          <div className="text-xs text-muted-foreground font-mono">
                            {model.model_id}
                          </div>
                        </div>
                      </div>
                    </td>
                    <td className="px-6 py-4">
                      <div className="flex items-center gap-2">
                        <span className="text-muted-foreground">
                          {modelType}
                        </span>
                        {model.variant && (
                          <Badge variant="outline" className="text-xs">
                            {model.variant}
                          </Badge>
                        )}
                        <Badge
                          variant="outline"
                          className="text-xs max-w-[150px] truncate text-muted-foreground"
                          title={
                            model.source === 'disk'
                              ? 'system preset'
                              : 'manually imported'
                          }
                        >
                          {model.source === 'disk'
                            ? t('sys.ai_models.provenance.system')
                            : t('sys.ai_models.provenance.manual')}
                        </Badge>
                      </div>
                    </td>
                    <td className="px-6 py-4">
                      <Badge
                        variant={isLoaded ? 'default' : 'secondary'}
                        className={`text-xs ${isLoaded ? 'bg-emerald-600 hover:bg-emerald-700' : ''}`}
                      >
                        {isLoaded
                          ? t('sys.ai_models.status.loaded', '已加载')
                          : t('sys.ai_models.status.uploaded', '未加载')}
                      </Badge>
                    </td>
                    <td className="px-6 py-4 text-muted-foreground whitespace-nowrap">
                      {model.input_width && model.input_height
                        ? `${model.input_width}×${model.input_height}`
                        : '-'}
                    </td>
                    <td className="px-6 py-4 text-muted-foreground whitespace-nowrap">
                      {formatFileSize(model.file_size)}
                    </td>
                    <td className="px-6 py-4 min-w-0 max-w-[200px]">
                      {model.model_path ? (
                        <TruncateWithTooltip
                          value={model.model_path}
                          className="block w-full text-muted-foreground text-sm font-mono"
                          tooltipClassName="max-w-md"
                          tooltipContentClassName="break-all"
                        />
                      ) : (
                        <span className="text-muted-foreground">-</span>
                      )}
                    </td>
                    <td className="px-6 py-4">
                      <div className="flex items-center gap-2">
                        <span className="text-muted-foreground">
                          {formatLoadTime(model.load_timestamp, t)}
                        </span>
                        {appsCount > 0 && (
                          <Badge variant="secondary" className="text-xs gap-1">
                            <AppWindow className="w-3 h-3" />
                            {appsCount}
                          </Badge>
                        )}
                      </div>
                    </td>
                    <td className="sticky right-0 z-9 bg-card px-6 py-4 text-start shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.1)] transition-colors dark:bg-background dark:shadow-[-4px_0_6px_-2px_rgba(0,0,0,0.3)]">
                      <div className="flex items-center gap-1 text-muted-foreground">
                        {isLoaded ? (
                          <Button
                            variant="ghost"
                            size="icon"
                            className="h-8 w-8 hover:text-orange-500"
                            disabled={isLoading}
                            onClick={() => setUnloadConfirm({
                                id: model.model_id,
                                name: model.name || model.model_id,
                              })}
                            title={t('sys.ai_models.action.unload', '卸载')}
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
                            size="icon"
                            className="h-8 w-8 hover:text-emerald-500"
                            disabled={isLoading}
                            onClick={() => onLoad(model.model_id)}
                            title={t('sys.ai_models.action.load', '加载')}
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
                          size="icon"
                          className="h-8 w-8 hover:text-foreground"
                          onClick={() => setDetailModel(model)}
                          title={t('common.detail', '详情')}
                        >
                          <Eye className="w-4 h-4" />
                        </Button>
                        {onUpdate && (
                          <Button
                            variant="ghost"
                            size="icon"
                            className="h-8 w-8 hover:text-primary"
                            onClick={() => {
                              if (appsCount > 0) {
                                setUpdateConfirm(model);
                              } else {
                                onUpdate(model);
                              }
                            }}
                            title={t('sys.ai_models.action.update', '更新')}
                          >
                            <Pencil className="w-4 h-4" />
                          </Button>
                        )}
                        <Button
                          variant="ghost"
                          size="icon"
                          className="h-8 w-8 hover:text-red-600 dark:hover:text-red-500"
                          onClick={() => handleDeleteClick(model)}
                          title={t('common.delete', '删除')}
                        >
                          <Trash2 className="w-4 h-4" />
                        </Button>
                      </div>
                    </td>
                  </tr>
                );
              })
            )}
          </tbody>
        </table>

        {/* Pagination */}
        <div className="px-6 py-4 border-t border-border flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between text-sm text-muted-foreground bg-background/80 dark:bg-muted/20">
          <span>
            {t(
              'sys.ai_models.pagination',
              '显示 {{start}} - {{end}} 共 {{total}} 个模型',
              {
                start: models.length === 0 ? 0 : startIndex + 1,
                end: Math.min(startIndex + PAGE_SIZE, models.length),
                // Unfiltered count — the filtered slice must not shrink the
                // reported total when the user is searching.
                total: totalCount ?? models.length,
              }
            )}
          </span>
          <div className="flex gap-1">
            <Button
              variant="outline"
              size="icon"
              className="w-8 h-8 rounded-md disabled:opacity-50"
              disabled={safePage <= 1}
              onClick={() => setCurrentPage(prev => Math.max(1, prev - 1))}
            >
              <ChevronLeft className="w-4 h-4" />
            </Button>
            <Button
              variant="default"
              size="icon"
              className="w-8 h-8 rounded-md bg-primary text-primary-foreground hover:bg-primary/90"
            >
              {safePage}
            </Button>
            <Button
              variant="outline"
              size="icon"
              className="w-8 h-8 rounded-md disabled:opacity-50"
              disabled={safePage >= totalPages}
              onClick={() => setCurrentPage(prev => Math.min(totalPages, prev + 1))}
            >
              <ChevronRight className="w-4 h-4" />
            </Button>
          </div>
        </div>
      </div>

      {/* Delete Confirmation */}
      <AlertDialog open={deleteDialogOpen} onOpenChange={setDeleteDialogOpen}>
        <AlertDialogContent>
          <AlertDialogHeader>
            <AlertDialogTitle>
              {t('sys.ai_models.confirm.delete_title', '确认删除')}
            </AlertDialogTitle>
            <AlertDialogDescription>
              {modelToDelete?.usedByApps
              && modelToDelete.usedByApps.length > 0 ? (
                <>
                  <span>
                    {t(
                      'sys.ai_models.message.delete_blocked',
                      '该模型正在被以下应用引用（含未运行应用），请先删除引用关系后再删除模型：'
                    )}
                  </span>
                  <ul className="mt-2 space-y-1">
                    {modelToDelete.usedByApps.map(app => (
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
                t(
                  'sys.ai_models.confirm.delete',
                  '确认删除模型 "{{name}}" 吗？此操作无法撤销。',
                  {
                    name: modelToDelete?.name,
                  }
                )
              )}
            </AlertDialogDescription>
          </AlertDialogHeader>
          <AlertDialogFooter>
            <AlertDialogCancel>{t('common.cancel', '取消')}</AlertDialogCancel>
            {(!modelToDelete?.usedByApps
              || modelToDelete.usedByApps.length === 0) && (
              <AlertDialogAction
                className="bg-destructive text-destructive-foreground hover:bg-destructive/90"
                onClick={confirmDelete}
              >
                {t('common.delete', '删除')}
              </AlertDialogAction>
            )}
          </AlertDialogFooter>
        </AlertDialogContent>
      </AlertDialog>

      {/* Unload Confirmation */}
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

      {/* Update Confirmation (model in use by apps) */}
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

      {/* Detail Dialog */}
      <ModelDetailDialog
        model={detailModel}
        open={!!detailModel}
        onOpenChange={open => !open && setDetailModel(null)}
        onLoad={onLoad}
        onUnload={onUnload}
        onUpdateFile={onUpdate}
        isActionLoading={isActionLoading}
      />
    </>
  );
}
