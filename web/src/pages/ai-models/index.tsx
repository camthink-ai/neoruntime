import { useState, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import {
  Select,
  SelectContent,
  SelectItem,
  SelectTrigger,
  SelectValue,
} from '@/components/ui/select';
import {
  LayoutGrid,
  List,
  Plus,
  Search,
  FolderSearch,
  ArrowUpDown,
} from 'lucide-react';
import {
  useModels,
  useUnregisterModel,
  useLoadModel,
  useUnloadModel,
  useScanModels,
} from '@/hooks/useModels';
import ModelCard from './components/ModelCard';
import ModelList from './components/ModelList';
import ImportModelDialog from './components/importModelDialog';
import { toast } from 'sonner';
import { AIModelsPageSkeleton } from './components/AIModelsSkeleton';
import { resolveModelType, type ModelTypeKey } from './utils';
import { apiErrorText } from './lib/apiErrors';

type StatusFilter = 'all' | 'loaded' | 'unloaded';
type SortBy = 'default' | 'name' | 'size' | 'load_time';

const SORT_OPTIONS: SortBy[] = ['default', 'name', 'size', 'load_time'];

/** Keyword match across identity, provenance and label fields. Labels
 *  arrive as an array or a comma-joined string depending on backend shape. */
const matchesKeyword = (model: any, keyword: string): boolean => {
  if (!keyword) return true;
  const fields = [
    model?.model_id,
    model?.name,
    model?.network_name,
    model?.source,
  ]
    .filter(Boolean)
    .map(v => String(v).toLowerCase());
  const labels = Array.isArray(model?.labels)
    ? model.labels.map((l: unknown) => String(l).toLowerCase())
    : typeof model?.labels === 'string'
      ? [model.labels.toLowerCase()]
      : [];
  return [...fields, ...labels].some(v => v.includes(keyword));
};

export default function AIModels() {
  const { t } = useTranslation();
  const [viewMode, setViewMode] = useState<'card' | 'list'>('card');
  const [search, setSearch] = useState('');
  const [typeFilter, setTypeFilter] = useState<ModelTypeKey | 'all'>('all');
  const [statusFilter, setStatusFilter] = useState<StatusFilter>('all');
  const [sortBy, setSortBy] = useState<SortBy>('default');
  const [importDialogOpen, setImportDialogOpen] = useState(false);
  const [updateDialogOpen, setUpdateDialogOpen] = useState(false);
  const [updateTarget, setUpdateTarget] = useState<any | null>(null);
  // Concurrent load/unload requests each pin their model here; the pages
  // consume it as an isActionLoading(id) predicate.
  const [loadingActions, setLoadingActions] = useState<Set<string>>(
    () => new Set()
  );
  const [scanning, setScanning] = useState(false);

  const { data: models = [], isLoading, error, refetch } = useModels();
  const unregisterModel = useUnregisterModel();
  const loadModel = useLoadModel();
  const unloadModel = useUnloadModel();
  const scanModels = useScanModels();

  /** Types actually present in the data — the filter never offers a chip
      that would match nothing. */
  const availableTypes = useMemo(() => {
    const keys = new Set<ModelTypeKey>();
    for (const model of models as any[]) {
      const key = resolveModelType(model?.model_type, model?.model_id);
      if (key) keys.add(key);
    }
    return [...keys].sort();
  }, [models]);

  const sortedModels = useMemo(() => {
    const list = [...(models as any[])];
    switch (sortBy) {
      case 'name':
        return list.sort((a, b) => String(a?.name || a?.model_id || '').localeCompare(
            String(b?.name || b?.model_id || '')
          ));
      case 'size':
        return list.sort(
          (a, b) => Number(b?.file_size ?? 0) - Number(a?.file_size ?? 0)
        );
      case 'load_time':
        return list.sort(
          (a, b) => Number(b?.load_timestamp ?? 0) - Number(a?.load_timestamp ?? 0)
        );
      default:
        return list.sort((a, b) => {
          const as = a?.status === 'loaded' ? 1 : 0;
          const bs = b?.status === 'loaded' ? 1 : 0;
          if (as !== bs) return bs - as;
          const at = Number(a?.load_timestamp ?? 0);
          const bt = Number(b?.load_timestamp ?? 0);
          return bt - at;
        });
    }
  }, [models, sortBy]);

  const filteredModels = useMemo(() => {
    const keyword = search.trim().toLowerCase();
    return sortedModels.filter(
      (model: any) => (typeFilter === 'all'
          || resolveModelType(model.model_type, model.model_id) === typeFilter)
        && (statusFilter === 'all'
          || (statusFilter === 'loaded'
            ? model.status === 'loaded'
            : model.status !== 'loaded'))
        && matchesKeyword(model, keyword)
    );
  }, [sortedModels, search, typeFilter, statusFilter]);

  const hasActiveFilters =    search.trim() !== '' || typeFilter !== 'all' || statusFilter !== 'all';

  const handleClearFilters = () => {
    setSearch('');
    setTypeFilter('all');
    setStatusFilter('all');
  };

  const handleDeleteModel = async (modelId: string, modelName: string) => {
    try {
      await unregisterModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.delete_success', `模型 "${modelName}" 已删除`)
      );
    } catch (err: any) {
      toast.error(
        apiErrorText(err, t('sys.ai_models.message.delete_failed', 'Failed to delete model'))
      );
    }
  };

  const handleLoadModel = async (modelId: string) => {
    setLoadingActions(prev => new Set(prev).add(modelId));
    try {
      await loadModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.load_success', '模型已加载到 NPU')
      );
    } catch (err: any) {
      toast.error(
        apiErrorText(err, t('sys.ai_models.message.load_failed', '加载失败'))
      );
    } finally {
      setLoadingActions(prev => {
        const next = new Set(prev);
        next.delete(modelId);
        return next;
      });
    }
  };

  const handleUnloadModel = async (modelId: string, _modelName: string) => {
    setLoadingActions(prev => new Set(prev).add(modelId));
    try {
      await unloadModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.unload_success', '模型已从 NPU 卸载')
      );
    } catch (err: any) {
      toast.error(
        apiErrorText(err, t('sys.ai_models.message.unload_failed', '卸载失败'))
      );
    } finally {
      setLoadingActions(prev => {
        const next = new Set(prev);
        next.delete(modelId);
        return next;
      });
    }
  };

  const handleScanModels = async () => {
    setScanning(true);
    try {
      const result = await scanModels.mutateAsync();
      if (result?.added > 0) {
        toast.success(
          t(
            'sys.ai_models.action.scan_success',
            'Scan complete',
            result
          ) as string
        );
      } else {
        toast.info(
          t(
            'sys.ai_models.action.scan_no_new',
            'No new models',
            result
          ) as string
        );
      }
    } catch (err: unknown) {
      const msg = err instanceof Error ? err.message : 'Scan failed';
      toast.error(msg);
    } finally {
      setScanning(false);
    }
  };

  const handleOpenUpdateDialog = (model: any) => {
    setUpdateTarget(model);
    setUpdateDialogOpen(true);
  };

  if (isLoading) {
    return <AIModelsPageSkeleton viewMode={viewMode} />;
  }

  // A failed list must not masquerade as an empty library — surface the
  // error with a retry instead of the "no models" intake hints.
  if (error) {
    return (
      <div className="p-4 md:p-6 mx-auto max-w-[1600px]">
        <div className="flex flex-col items-center gap-3 rounded-lg border border-dashed border-border bg-muted/20 px-6 py-12 text-center">
          <p className="text-sm font-medium text-foreground">
            {t('sys.ai_models.error.load_failed', '模型列表加载失败')}
          </p>
          <p className="text-xs text-muted-foreground">
            {error instanceof Error ? error.message : String(error)}
          </p>
          <Button
            type="button"
            variant="outline"
            size="sm"
            onClick={() => refetch()}
          >
            {t('common.retry', '重试')}
          </Button>
        </div>
      </div>
    );
  }

  const statusChips: { value: StatusFilter; label: string }[] = [
    { value: 'all', label: t('sys.ai_models.filter.all_status', '全部状态') },
    { value: 'loaded', label: t('sys.ai_models.status.loaded', '已加载') },
    { value: 'unloaded', label: t('sys.ai_models.status.uploaded', '未加载') },
  ];

  const typeChips: { value: ModelTypeKey | 'all'; label: string }[] = [
    { value: 'all', label: t('sys.ai_models.filter.all_types', '全部类型') },
    ...availableTypes.map(key => ({
      value: key,
      label: t(`sys.ai_models.model_type.${key}`),
    })),
  ];

  const renderChip = (active: boolean, label: string, onClick: () => void) => (
    <Button
      key={label}
      type="button"
      variant={active ? 'default' : 'outline'}
      size="sm"
      className="rounded-full px-4 h-8"
      onClick={onClick}
    >
      {label}
    </Button>
  );

  return (
    <div className="p-4 md:p-6 mx-auto max-w-[1600px]">
      {/* Toolbar — identical in both view modes so the primary intake
          actions never depend on the view toggle. */}
      <div className="flex flex-wrap items-center gap-3 md:gap-4 mb-4">
        <div className="flex items-center border rounded-lg p-1">
          <Button
            variant={viewMode === 'card' ? 'secondary' : 'ghost'}
            size="sm"
            onClick={() => setViewMode('card')}
            className="px-3"
          >
            <LayoutGrid className="w-4 h-4" />
          </Button>
          <Button
            variant={viewMode === 'list' ? 'secondary' : 'ghost'}
            size="sm"
            onClick={() => setViewMode('list')}
            className="px-3"
          >
            <List className="w-4 h-4" />
          </Button>
        </div>

        <div className="relative min-w-0 flex-1 max-w-md basis-[min(100%,28rem)]">
          <Search className="absolute left-3 top-1/2 -translate-y-1/2 h-4 w-4 text-muted-foreground" />
          <Input
            placeholder={t(
              'sys.ai_models.action.search_placeholder',
              'Search models...'
            )}
            className="pl-9"
            value={search}
            onChange={e => setSearch(e.target.value)}
          />
        </div>

        <Select
          value={sortBy}
          onValueChange={value => setSortBy(value as SortBy)}
        >
          <SelectTrigger className="w-[170px] shrink-0">
            <ArrowUpDown className="w-4 h-4 mr-1 text-muted-foreground" />
            <SelectValue />
          </SelectTrigger>
          <SelectContent>
            {SORT_OPTIONS.map(option => (
              <SelectItem key={option} value={option}>
                {t(`sys.ai_models.sort.${option}`)}
              </SelectItem>
            ))}
          </SelectContent>
        </Select>

        <div className="flex flex-wrap items-center gap-3 ml-auto max-sm:ml-0 max-sm:w-full">
          <Button
            type="button"
            variant="outline"
            className="shrink-0"
            disabled={scanning}
            onClick={handleScanModels}
          >
            <FolderSearch className="w-4 h-4 mr-2" />
            {scanning
              ? t('sys.ai_models.action.scanning', 'Scanning...')
              : t('sys.ai_models.action.scan', 'Scan Models')}
          </Button>
          <Button
            type="button"
            variant="carbon"
            className="shrink-0"
            onClick={() => setImportDialogOpen(true)}
          >
            <Plus className="w-4 h-4 mr-2" />
            {t('sys.ai_models.action.import', 'Import Model')}
          </Button>
        </div>
      </div>

      {/* Filter chips — status first (coarsest cut), then type. Only the
          types present in the data are offered. */}
      {models.length > 0 && (
        <div className="flex flex-wrap items-center gap-2 mb-6">
          {statusChips.map(chip => renderChip(statusFilter === chip.value, chip.label, () => setStatusFilter(chip.value)))}
          <span className="mx-1 h-5 w-px bg-border" aria-hidden />
          {typeChips.map(chip => renderChip(typeFilter === chip.value, chip.label, () => setTypeFilter(chip.value)))}
          {hasActiveFilters && (
            <Button
              type="button"
              variant="ghost"
              size="sm"
              className="h-8 px-3 text-muted-foreground"
              onClick={handleClearFilters}
            >
              {t('sys.ai_models.empty.clear_filters', '清除筛选')}
            </Button>
          )}
        </div>
      )}

      {/* Content */}
      <div>
        {viewMode === 'card' ? (
          <ModelCard
            models={filteredModels}
            totalCount={models.length}
            onClearFilters={handleClearFilters}
            onDelete={handleDeleteModel}
            onLoad={handleLoadModel}
            onUnload={handleUnloadModel}
            onImportClick={() => setImportDialogOpen(true)}
            onUpdate={handleOpenUpdateDialog}
            isActionLoading={id => loadingActions.has(id)}
          />
        ) : (
          <ModelList
            models={filteredModels}
            totalCount={models.length}
            onClearFilters={handleClearFilters}
            onDelete={handleDeleteModel}
            onLoad={handleLoadModel}
            onUnload={handleUnloadModel}
            onUpdate={handleOpenUpdateDialog}
            isActionLoading={id => loadingActions.has(id)}
          />
        )}
      </div>

      <ImportModelDialog
        open={importDialogOpen}
        onOpenChange={setImportDialogOpen}
        onSuccess={refetch}
      />

      <ImportModelDialog
        open={updateDialogOpen}
        onOpenChange={setUpdateDialogOpen}
        onSuccess={refetch}
        mode="update"
        model={updateTarget}
      />
    </div>
  );
}
