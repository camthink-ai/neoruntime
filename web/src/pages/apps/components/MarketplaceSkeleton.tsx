import { Skeleton } from '@/components/ui/skeleton';

export function MarketplaceSkeleton() {
  return (
    <div className="grid grid-cols-1 md:grid-cols-2 xl:grid-cols-3 2xl:grid-cols-4 gap-4 mt-6">
      {/* Import card placeholder */}
      <div className="flex flex-col items-center justify-center p-6 border-2 border-dashed border-border rounded-lg min-h-[220px]">
        <Skeleton className="w-16 h-16 rounded-full mb-4" />
        <Skeleton className="h-5 w-24 mb-2" />
        <Skeleton className="h-4 w-36" />
      </div>

      {/* App card placeholders */}
      {[1, 2, 3, 4, 5].map(i => (
        <div
          key={i}
          className="flex flex-col p-6 justify-between rounded-lg border shadow-sm min-h-[220px]"
        >
          <div className="flex flex-col gap-4">
            <div className="flex items-start justify-between">
              <Skeleton className="w-12 h-12 rounded-xl" />
              <Skeleton className="h-6 w-16 rounded-full" />
            </div>
            <div>
              <Skeleton className="h-5 w-28 mb-2" />
              <Skeleton className="h-4 w-full" />
              <Skeleton className="h-4 w-3/4 mt-1" />
            </div>
            <Skeleton className="h-3 w-20" />
          </div>
          <div>
            <div className="h-px bg-border my-2" />
            <div className="flex items-center justify-end gap-1 mt-2">
              <Skeleton className="h-8 w-8 rounded-md" />
              <Skeleton className="h-8 w-8 rounded-md" />
              <Skeleton className="h-8 w-8 rounded-md" />
            </div>
          </div>
        </div>
      ))}
    </div>
  );
}
