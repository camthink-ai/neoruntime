import { useState, useMemo } from 'react';
import { useTranslation } from 'react-i18next';
import { Button } from '@/components/ui/button';
import { Input } from '@/components/ui/input';
import { LayoutGrid, List, Plus, Search, FolderSearch } from 'lucide-react';
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

export default function AIModels() {
  const { t } = useTranslation();
  const [viewMode, setViewMode] = useState<'card' | 'list'>('card');
  const [search, setSearch] = useState('');
  const [importDialogOpen, setImportDialogOpen] = useState(false);
  const [loadingAction, setLoadingAction] = useState<string | null>(null);
  const [scanning, setScanning] = useState(false);

  const { data: models = [], isLoading, refetch } = useModels();
  const unregisterModel = useUnregisterModel();
  const loadModel = useLoadModel();
  const unloadModel = useUnloadModel();
  const scanModels = useScanModels();

  const sortedModels = useMemo(
    () => [...models].sort((a: any, b: any) => {
        const as = a?.status === 'loaded' ? 1 : 0;
        const bs = b?.status === 'loaded' ? 1 : 0;
        if (as !== bs) return bs - as;
        const at = Number(a?.load_timestamp ?? 0);
        const bt = Number(b?.load_timestamp ?? 0);
        return bt - at;
      }),
    [models]
  );

  const filteredModels = useMemo(() => {
    const keyword = search.trim().toLowerCase();
    if (!keyword) return sortedModels;
    return sortedModels.filter(
      (model: any) => model.model_id?.toLowerCase().includes(keyword)
        || model.name?.toLowerCase().includes(keyword)
    );
  }, [sortedModels, search]);

  const handleDeleteModel = async (modelId: string, modelName: string) => {
    try {
      await unregisterModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.delete_success', `模型 "${modelName}" 已删除`)
      );
    } catch (error: any) {
      toast.error(
        error?.response?.data?.message
          || t('sys.ai_models.message.delete_failed', 'Failed to delete model')
      );
    }
  };

  const handleLoadModel = async (modelId: string) => {
    setLoadingAction(modelId);
    try {
      await loadModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.load_success', '模型已加载到 NPU')
      );
    } catch (error: any) {
      toast.error(
        error?.response?.data?.message
          || t('sys.ai_models.message.load_failed', '加载失败')
      );
    } finally {
      setLoadingAction(null);
    }
  };

  const handleUnloadModel = async (modelId: string, _modelName: string) => {
    setLoadingAction(modelId);
    try {
      await unloadModel.mutateAsync(modelId);
      toast.success(
        t('sys.ai_models.message.unload_success', '模型已从 NPU 卸载')
      );
    } catch (error: any) {
      toast.error(
        error?.response?.data?.message
          || t('sys.ai_models.message.unload_failed', '卸载失败')
      );
    } finally {
      setLoadingAction(null);
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
    } catch (error: unknown) {
      const msg = error instanceof Error ? error.message : 'Scan failed';
      toast.error(msg);
    } finally {
      setScanning(false);
    }
  };

  if (isLoading) {
    return <AIModelsPageSkeleton viewMode={viewMode} />;
  }

  return (
    <div className="p-4 md:p-6 mx-auto max-w-[1600px]">
      {/* Toolbar */}
      <div className="flex flex-wrap items-center gap-3 md:gap-4 mb-6">
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

        {viewMode === 'list' && (
          <>
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
              className="ml-auto max-sm:ml-0 max-sm:w-full max-sm:basis-full shrink-0"
              onClick={() => setImportDialogOpen(true)}
            >
              <Plus className="w-4 h-4 mr-2" />
              {t('sys.ai_models.action.import', 'Import Model')}
            </Button>
          </>
        )}

        {viewMode === 'card' && (
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
        )}
      </div>

      {/* Content */}
      <div>
        {viewMode === 'card' ? (
          <ModelCard
            models={filteredModels}
            onDelete={handleDeleteModel}
            onLoad={handleLoadModel}
            onUnload={handleUnloadModel}
            onImportClick={() => setImportDialogOpen(true)}
            loadingAction={loadingAction}
          />
        ) : (
          <ModelList
            models={filteredModels}
            onDelete={handleDeleteModel}
            onLoad={handleLoadModel}
            onUnload={handleUnloadModel}
            loadingAction={loadingAction}
          />
        )}
      </div>

      <ImportModelDialog
        open={importDialogOpen}
        onOpenChange={setImportDialogOpen}
        onSuccess={refetch}
      />
    </div>
  );
}
