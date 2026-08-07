import { Skeleton } from '@/components/ui/skeleton';
import { Card, CardContent } from '@/components/ui/card';

export default function ImageSettingsSkeleton() {
  return (
    <div className="space-y-6">
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-4 p-5">
          <div className="flex items-center gap-2">
            <Skeleton className="h-3.5 w-3.5 rounded-sm" />
            <Skeleton className="h-4 w-24" />
          </div>
          <div className="flex items-center justify-between">
            <div className="space-y-2">
              <Skeleton className="h-3 w-20" />
              <Skeleton className="h-3 w-48" />
            </div>
            <Skeleton className="h-6 w-10 rounded-full" />
          </div>
          {[1, 2, 3].map(i => (
            <div key={i} className="space-y-2">
              <Skeleton className="h-3 w-16" />
              <div className="flex items-center gap-3">
                <Skeleton className="h-2 flex-1 rounded-md" />
                <Skeleton className="h-3 w-10" />
              </div>
            </div>
          ))}
          <div className="space-y-2">
            <Skeleton className="h-3 w-24" />
            <Skeleton className="h-10 w-full rounded-xl" />
          </div>
          <div className="space-y-2">
            <Skeleton className="h-3 w-20" />
            <Skeleton className="h-10 w-full rounded-xl" />
          </div>
        </CardContent>
      </Card>

      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-4 p-5">
          <div className="flex items-center gap-2">
            <Skeleton className="h-3.5 w-3.5 rounded-sm" />
            <Skeleton className="h-4 w-20" />
          </div>
          {[1, 2].map(i => (
            <div key={i} className="space-y-2">
              <Skeleton className="h-3 w-16" />
              <Skeleton className="h-10 w-full rounded-xl" />
            </div>
          ))}
          {[1, 2].map(i => (
            <div key={i} className="flex items-center justify-between">
              <Skeleton className="h-3 w-20" />
              <Skeleton className="h-6 w-10 rounded-full" />
            </div>
          ))}
        </CardContent>
      </Card>
    </div>
  );
}
