import { Skeleton } from '@/components/ui/skeleton';
import { useTranslation } from 'react-i18next';

interface AIModelsPageSkeletonProps {
  viewMode: 'card' | 'list';
}

export function AIModelsPageSkeleton({ viewMode }: AIModelsPageSkeletonProps) {
  return (
    <div className="p-4 md:p-6 mx-auto max-w-[1600px]">
      <AIModelsToolbarSkeleton viewMode={viewMode} />
      {viewMode === 'card' ? (
        <AIModelsCardSkeleton />
      ) : (
        <AIModelsListSkeleton />
      )}
    </div>
  );
}

function AIModelsToolbarSkeleton({ viewMode }: { viewMode: 'card' | 'list' }) {
  return (
    <div className="flex flex-wrap items-center gap-3 md:gap-4 mb-6">
      <div className="flex items-center border rounded-lg p-1">
        <Skeleton className="h-8 w-9 rounded-md" />
        <Skeleton className="h-8 w-9 rounded-md" />
      </div>
      <Skeleton className="h-10 min-w-0 flex-1 max-w-md basis-[min(100%,28rem)] rounded-md" />
      {viewMode === 'list' ? (
        <>
          <Skeleton className="h-10 w-28 shrink-0 rounded-md" />
          <Skeleton className="h-10 w-32 shrink-0 rounded-md max-sm:ml-0 max-sm:w-full max-sm:basis-full" />
        </>
      ) : (
        <Skeleton className="h-10 w-28 shrink-0 rounded-md" />
      )}
    </div>
  );
}

export function AIModelsCardSkeleton() {
  return (
    <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4">
      {[1, 2, 3, 4, 5, 6].map(i => (
        <div
          key={i}
          className="flex flex-col p-5 justify-between rounded-lg border shadow-sm min-h-[200px]"
        >
          <div className="flex flex-col gap-3">
            <div className="flex items-start justify-between">
              <Skeleton className="w-10 h-10 rounded-lg" />
              <Skeleton className="h-5 w-16 rounded-full" />
            </div>
            <div>
              <Skeleton className="h-4 w-32 mb-1" />
              <Skeleton className="h-3 w-20" />
            </div>
            <div className="space-y-1.5">
              <div className="flex justify-between">
                <Skeleton className="h-3 w-8" />
                <Skeleton className="h-3 w-20" />
              </div>
              <div className="flex justify-between">
                <Skeleton className="h-3 w-12" />
                <Skeleton className="h-3 w-16" />
              </div>
              <div className="flex justify-between">
                <Skeleton className="h-3 w-12" />
                <Skeleton className="h-3 w-14" />
              </div>
              <div className="flex justify-between">
                <Skeleton className="h-3 w-8" />
                <Skeleton className="h-3 w-24" />
              </div>
            </div>
          </div>
          <div className="flex justify-end gap-1 mt-4 pt-3 border-t">
            <Skeleton className="h-8 w-16 rounded-md" />
            <Skeleton className="h-8 w-16 rounded-md" />
          </div>
        </div>
      ))}
    </div>
  );
}

export function AIModelsListSkeleton() {
  const { t } = useTranslation();

  return (
    <div className="bg-card rounded-xl border border-border shadow-sm overflow-hidden">
      <table className="w-full min-w-[720px] text-sm text-left">
        <thead className="bg-background border-b border-border text-muted-foreground font-medium">
          <tr>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.model_id', 'Model ID')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.type', '类型')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.model_path', '路径')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.version', '版本')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.load_time', '加载时间')}
            </th>
            <th className="px-6 py-4 font-medium">
              {t('sys.ai_models.table.actions', '操作')}
            </th>
          </tr>
        </thead>
        <tbody>
          {[1, 2, 3, 4, 5].map(i => (
            <tr key={i} className="border-b border-border/60 last:border-b-0">
              <td className="px-6 py-4">
                <Skeleton className="h-4 w-28" />
              </td>
              <td className="px-6 py-4">
                <Skeleton className="h-5 w-16 rounded-full" />
              </td>
              <td className="px-6 py-4">
                <Skeleton className="h-4 w-40" />
              </td>
              <td className="px-6 py-4">
                <Skeleton className="h-4 w-12" />
              </td>
              <td className="px-6 py-4">
                <Skeleton className="h-4 w-20" />
              </td>
              <td className="px-6 py-4">
                <div className="flex justify-end gap-2">
                  <Skeleton className="h-8 w-8 rounded-md" />
                  <Skeleton className="h-8 w-8 rounded-md" />
                  <Skeleton className="h-8 w-8 rounded-md" />
                </div>
              </td>
            </tr>
          ))}
        </tbody>
      </table>
    </div>
  );
}
