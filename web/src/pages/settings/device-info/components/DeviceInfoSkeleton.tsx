import { Skeleton } from '@/components/ui/skeleton';
import { Card } from '@/components/ui/card';

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

          <Card className="overflow-hidden gap-0 p-0 shadow-sm">
            {/* Mobile: compact label | value table */}
            <div className="flex flex-col gap-3 p-4 md:hidden">
              {[1, 2, 3, 4, 5, 6].map(i => (
                <div key={i} className="flex items-center gap-3">
                  <Skeleton className="h-4 w-[30%]" />
                  <Skeleton className="h-5 w-28" />
                </div>
              ))}
            </div>

            {/* md+: grid cards */}
            <div className="hidden md:grid md:grid-cols-3 md:gap-x-6 md:gap-y-4 md:p-4 lg:p-6">
              {[1, 2, 3, 4, 5, 6].map(i => (
                <div key={i} className="flex flex-col gap-1">
                  <Skeleton className="h-4 w-16" />
                  <div className="flex min-h-8 items-center gap-2">
                    <Skeleton className="h-5 w-24" />
                    {i === 1 && <Skeleton className="h-6 w-6 rounded-md" />}
                  </div>
                </div>
              ))}
            </div>
          </Card>
        </section>

        {/* Section: 固件与硬件 */}
        <section className="space-y-4">
          <div className="flex items-center gap-2">
            <Skeleton className="w-5 h-5 rounded-sm" />
            <Skeleton className="h-6 w-28" />
          </div>

          <Card className="py-2 flex flex-col overflow-hidden gap-0 shadow-sm">
            {[1, 2, 3, 4, 5, 6].map(i => (
              <div
                key={i}
                className="flex flex-col gap-3 p-4 md:flex-row md:items-center md:justify-between md:gap-4 md:p-6"
              >
                <div className="flex min-w-0 items-center gap-4">
                  <Skeleton className="h-11 w-11 shrink-0 rounded-lg" />
                  <div className="flex min-w-0 flex-col gap-1">
                    <Skeleton className="h-4 w-24" />
                    <Skeleton className="h-4 w-40" />
                  </div>
                </div>
                {i <= 2 && <Skeleton className="hidden h-9 w-24 md:block" />}
              </div>
            ))}
          </Card>
        </section>

        {/* Section: Bottom Actions — mobile: stacked full-width, md+: right-aligned */}
        <div className="flex flex-col gap-4 pb-6 md:flex-row md:flex-wrap md:justify-end md:gap-4">
          <Skeleton className="h-11 w-full md:hidden" />
          <Skeleton className="h-11 w-full md:w-36" />
          <Skeleton className="h-11 w-full md:w-32" />
        </div>
      </div>
    </div>
  );
}
