import { Card, CardContent } from '@/components/ui/card';
import { Separator } from '@/components/ui/separator';
import { Skeleton } from '@/components/ui/skeleton';

export function TimeSettingsSkeleton() {
  return (
    <div className="p-6 md:p-12 max-w-4xl w-full min-h-screen bg-background mx-auto">
      <Card>
        <CardContent className="p-6 md:p-8 space-y-6">
          {/* Section 1: Current System Time */}
          <section>
            <div className="flex flex-col gap-6 sm:flex-row sm:items-start sm:justify-between">
              <div className="min-w-0 flex-1">
                <Skeleton className="h-10 w-48 sm:h-12 sm:w-56 md:h-14 md:w-64" />
                <div className="mt-2 flex items-center gap-2">
                  <Skeleton className="h-4 w-40" />
                  <Skeleton className="h-3 w-px" />
                  <Skeleton className="h-4 w-24" />
                </div>
              </div>
              <div className="flex shrink-0 items-center gap-2 sm:my-auto">
                <Skeleton className="h-4 w-4 rounded-sm" />
                <div className="space-y-1.5">
                  <Skeleton className="h-3 w-14" />
                  <Skeleton className="h-4 w-36" />
                </div>
              </div>
            </div>
          </section>

          <Separator />

          {/* Section 2: System Time Settings */}
          <section>
            <div className="flex items-center gap-2">
              <Skeleton className="h-[18px] w-[18px] rounded-sm" />
              <Skeleton className="h-5 w-40" />
            </div>

            <div className="space-y-6 pt-4">
              {/* Timezone */}
              <div className="space-y-2">
                <Skeleton className="h-4 w-16" />
                <Skeleton className="h-10 w-full max-w-lg rounded-md" />
              </div>

              {/* Time format */}
              <div className="space-y-2">
                <Skeleton className="h-4 w-20" />
                <Skeleton className="h-10 w-full max-w-md rounded-md" />
              </div>

              {/* DST */}
              <div className="flex flex-col gap-2">
                <Skeleton className="h-4 w-28" />
                <Skeleton className="h-6 w-10 rounded-full" />
              </div>

              {/* Sync mode */}
              <div className="space-y-2">
                <Skeleton className="h-4 w-20" />
                <div className="flex flex-wrap gap-6">
                  <div className="flex items-center gap-2">
                    <Skeleton className="h-4 w-4 rounded-full" />
                    <Skeleton className="h-4 w-24" />
                  </div>
                  <div className="flex items-center gap-2">
                    <Skeleton className="h-4 w-4 rounded-full" />
                    <Skeleton className="h-4 w-16" />
                  </div>
                </div>

                {/* NTP branch placeholder (matches typical expanded state) */}
                <div className="space-y-4 pt-2">
                  <div className="space-y-2">
                    <Skeleton className="h-3 w-24" />
                    <div className="flex flex-col gap-3 sm:flex-row sm:items-center">
                      <Skeleton className="h-10 w-full max-w-md rounded-md" />
                      <Skeleton className="h-9 w-28 rounded-md shrink-0" />
                    </div>
                  </div>
                  <div className="space-y-2">
                    <Skeleton className="h-3 w-24" />
                    <Skeleton className="h-10 w-full max-w-md rounded-md" />
                  </div>
                </div>
              </div>

              <Separator />

              <div className="flex justify-end pt-2">
                <Skeleton className="h-10 w-28 rounded-md" />
              </div>
            </div>
          </section>
        </CardContent>
      </Card>
    </div>
  );
}
