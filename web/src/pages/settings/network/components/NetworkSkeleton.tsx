import { Skeleton } from '@/components/ui/skeleton';

export function NetworkSkeleton() {
  return (
    <div className="p-6 md:p-12 space-y-6 max-w-4xl mx-auto">
      <div className="bg-card rounded-lg p-6">
        {/* Header */}
        <div className="flex items-center justify-between mb-8">
          <div className="flex items-center gap-2">
            <Skeleton className="w-5 h-5 rounded-sm" />
            <Skeleton className="h-6 w-24" />
          </div>
          {/* Interface info badge */}
          <div className="flex items-center gap-2 px-3 py-1.5">
            <Skeleton className="w-4 h-4 rounded-sm" />
            <Skeleton className="h-4 w-20" />
            <Skeleton className="h-3 w-24" />
          </div>
        </div>

        <div className="grid grid-cols-1 md:grid-cols-2 gap-6">
          {/* Mode selector */}
          <div className="col-span-1 md:col-span-2 space-y-2">
            <Skeleton className="h-4 w-10" />
            <Skeleton className="h-10 w-full" />
          </div>

          {/* IP Address */}
          <div className="space-y-2">
            <Skeleton className="h-4 w-16" />
            <Skeleton className="h-10 w-full" />
          </div>

          {/* Subnet Mask */}
          <div className="space-y-2">
            <Skeleton className="h-4 w-16" />
            <Skeleton className="h-10 w-full" />
          </div>

          {/* Gateway */}
          <div className="space-y-2">
            <Skeleton className="h-4 w-10" />
            <Skeleton className="h-10 w-full" />
          </div>

          {/* DNS */}
          <div className="space-y-2">
            <Skeleton className="h-4 w-20" />
            <Skeleton className="h-10 w-full" />
          </div>

          {/* DNS2 */}
          <div className="space-y-2">
            <Skeleton className="h-4 w-16" />
            <Skeleton className="h-10 w-full" />
          </div>
        </div>

        {/* Actions */}
        <div className="flex justify-end gap-4 mt-8">
          <Skeleton className="h-10 w-24" />
          <Skeleton className="h-10 w-20" />
        </div>
      </div>
    </div>
  );
}
