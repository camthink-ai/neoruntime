import { Skeleton } from '@/components/ui/skeleton';

export function DeviceInfoSkeleton() {
  return (
    <div className="p-6 md:p-12 w-full h-screen bg-background max-w-4xl mx-auto">
      <div className="space-y-10">
        {/* Section: 基本信息 */}
        <section className="space-y-4">
          <div className="flex items-center gap-2">
            <Skeleton className="w-5 h-5 rounded-full" />
            <Skeleton className="h-6 w-24" />
          </div>

          <div className="bg-card rounded-lg shadow-sm overflow-hidden">
            {/* Row 1 */}
            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3">
              {[1, 2, 3].map(i => (
                <div key={i} className="p-4 border-b border-r border-border/50">
                  <Skeleton className="h-4 w-16 mb-2" />
                  <Skeleton className="h-5 w-32" />
                </div>
              ))}
            </div>
            {/* Row 2 */}
            <div className="grid grid-cols-1 md:grid-cols-2 lg:grid-cols-3">
              {[4, 5, 6].map(i => (
                <div
                  key={i}
                  className="p-4 border-b border-r border-border/50 last:border-r-0"
                >
                  <Skeleton className="h-4 w-16 mb-2" />
                  <Skeleton className="h-5 w-32" />
                </div>
              ))}
            </div>
          </div>
        </section>

        {/* Section: 固件与硬件 */}
        <section className="space-y-4">
          <div className="flex items-center gap-2">
            <Skeleton className="w-5 h-5 rounded-sm" />
            <Skeleton className="h-6 w-28" />
          </div>

          <div className="bg-card rounded-lg shadow-sm flex flex-col overflow-hidden">
            {[1, 2, 3, 4, 5].map(i => (
              <div
                key={i}
                className="flex items-center gap-4 p-4 border-b border-border/50 last:border-b-0"
              >
                <Skeleton className="w-5 h-5 shrink-0" />
                <div className="flex-1 min-w-0">
                  <Skeleton className="h-4 w-24 mb-2" />
                  <Skeleton className="h-4 w-40" />
                </div>
              </div>
            ))}
          </div>
        </section>

        {/* Section: Bottom Actions */}
        <div className="flex flex-wrap gap-4 pt-2">
          {[1, 2, 3].map(i => (
            <Skeleton key={i} className="h-11 w-28 rounded-md" />
          ))}
        </div>
      </div>
    </div>
  );
}
