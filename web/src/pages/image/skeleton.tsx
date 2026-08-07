import { Skeleton } from '@/components/ui/skeleton';
import { Card, CardContent } from '@/components/ui/card';

export default function DeviceToolSkeleton() {
  return (
    <div className="space-y-6 p-2 max-w-full overflow-hidden">
      {/* Page title skeleton */}
      {/* <div className="space-y-2">
                <Card className="bg-white">
                    <CardContent className="pt-6">
                        <div className="space-y-4">
                            <div className="flex items-center justify-between gap-4">
                                <Skeleton className="h-5 w-20 sm:w-24 flex-shrink-0" />
                                <Skeleton className="h-6 w-12 flex-shrink-0" />
                            </div>
                            <Skeleton className="h-4 w-full max-w-sm" />
                        </div>
                    </CardContent>
                </Card>
            </div> */}

      {/* AI inference settings skeleton */}
      <div className="space-y-2">
        <Skeleton className="h-5 w-16 sm:w-20 bg-gray-200" />
        <Card className="bg-white">
          <CardContent className="pt-6">
            <div className="space-y-4">
              <div className="flex items-center justify-between gap-4">
                <Skeleton className="h-5 w-20 sm:w-24 shrink-0" />
                <Skeleton className="h-6 w-12 shrink-0" />
              </div>
              <Skeleton className="h-4 w-full max-w-sm" />
              <Skeleton className="h-4 w-full max-w-sm" />
              <Skeleton className="h-4 w-full max-w-sm" />
            </div>
          </CardContent>
        </Card>
      </div>

      {/* Camera settings skeleton */}
      <div className="space-y-2">
        <Skeleton className="h-5 w-16 sm:w-20 bg-gray-200" />
        <Card className="bg-white">
          <CardContent className="pt-6">
            <div className="space-y-4">
              {/* Model upload */}
              <div className="flex items-center justify-between gap-4">
                <Skeleton className="h-5 w-12 sm:w-16 shrink-0" />
                <Skeleton className="h-10 w-20 sm:w-24 shrink-0" />
              </div>

              {/* Edit button */}
              <div className="flex items-center justify-between gap-4">
                <Skeleton className="h-5 w-12 sm:w-16 shrink-0" />
                <Skeleton className="h-10 w-16 sm:w-20 shrink-0" />
              </div>
            </div>
          </CardContent>
        </Card>
      </div>
    </div>
  );
}
