import { Skeleton } from '@/components/ui/skeleton';
import { Card, CardContent } from '@/components/ui/card';

export default function MediaSettingsSkeleton() {
  return (
    <div className="p-4 space-y-4">
      {/* 1. 码流设置 + 码流配置（合并到一个 Card） */}
      <section className="space-y-4">
        <Card className="shadow-sm bg-background">
          <CardContent className="p-6 space-y-5">
            {/* 标题 */}
            <div className="flex items-center gap-1.5">
              <Skeleton className="w-3.5 h-3.5 rounded-sm" />
              <Skeleton className="h-4 w-28" />
            </div>

            {/* 码流选择 — 横向等宽按钮（flex-1），每个含名称 + 分辨率 */}
            <div className="flex gap-2">
              {[1, 2, 3].map(i => (
                <div
                  key={i}
                  className="flex-1 rounded-md border border-border px-3 py-2 text-center space-y-1"
                >
                  <Skeleton className="h-3 w-10 mx-auto" />
                  <Skeleton className="h-2.5 w-14 mx-auto" />
                </div>
              ))}
            </div>

            <Skeleton className="h-px w-full" />

            {/* 启用码流开关 */}
            <div className="flex items-center justify-between gap-3">
              <div className="space-y-1.5">
                <Skeleton className="h-3.5 w-24" />
                <Skeleton className="h-3 w-40" />
              </div>
              <Skeleton className="h-6 w-10 rounded-full" />
            </div>

            {/* 配置字段 grid（codec / 分辨率 / fps / 码率 …） */}
            <div className="grid grid-cols-1 md:grid-cols-2 gap-4">
              {[1, 2, 3, 4].map(i => (
                <div key={i} className="space-y-2">
                  <Skeleton className="h-3 w-14" />
                  <Skeleton className="h-9 w-full rounded-md" />
                </div>
              ))}
            </div>
          </CardContent>
        </Card>
      </section>

      {/* 2. RTSP 服务 */}
      <section className="space-y-4">
        <Card className="shadow-sm bg-background">
          <CardContent className="p-5 space-y-3">
            <div className="flex items-center gap-1.5">
              <Skeleton className="w-3.5 h-3.5 rounded-sm" />
              <Skeleton className="h-4 w-20" />
            </div>
            <div className="flex items-center justify-between">
              <Skeleton className="h-3 w-32" />
              <Skeleton className="h-6 w-10 rounded-full" />
            </div>
            <Skeleton className="h-3 w-48" />
            <div className="flex items-center gap-2">
              <Skeleton className="h-10 flex-1 rounded-xl" />
              <Skeleton className="h-10 w-10 rounded-md" />
            </div>
          </CardContent>
        </Card>
      </section>
    </div>
  );
}
