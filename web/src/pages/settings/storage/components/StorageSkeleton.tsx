import { Skeleton } from '@/components/ui/skeleton';

export function StorageSkeleton() {
  return (
    <div className="p-5 md:p-10 max-w-4xl w-full min-h-screen bg-background mx-auto">
      {/* Header */}
      <div className="flex items-center justify-between mb-6">
        <Skeleton className="h-5 w-48" />
        <Skeleton className="h-8 w-16" />
      </div>

      <div className="flex flex-col gap-4">
        {/* Internal Storage Card skeleton */}
        <div className="bg-card rounded-xl border shadow-sm p-6 space-y-5">
          <div className="flex items-center gap-6">
            <Skeleton className="w-28 h-28 rounded-full flex-shrink-0" />
            <div>
              <Skeleton className="h-4 w-24 mb-2" />
              <Skeleton className="h-3 w-36" />
            </div>
          </div>
          <div className="grid grid-cols-2 gap-2">
            <Skeleton className="h-14 rounded-lg" />
            <Skeleton className="h-14 rounded-lg" />
          </div>
        </div>

        {/* SD Card / Empty slot skeleton */}
        <div className="bg-card rounded-xl border shadow-sm p-6 space-y-5">
          <div className="flex items-center gap-3">
            <Skeleton className="w-10 h-10 rounded-lg" />
            <div>
              <Skeleton className="h-4 w-20 mb-2" />
              <Skeleton className="h-3 w-28" />
            </div>
          </div>
          <Skeleton className="h-2 w-full rounded-full" />
          <div className="grid grid-cols-2 gap-2">
            <Skeleton className="h-14 rounded-lg" />
          </div>
        </div>
      </div>
    </div>
  );
}
