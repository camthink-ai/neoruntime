import { Skeleton } from '@/components/ui/skeleton';
import { Card, CardContent } from '@/components/ui/card';

export function MotorAxisRowSkeleton() {
  return (
    <div className="space-y-2">
      <div className="flex items-center justify-between">
        <Skeleton className="h-4 w-16" />
        <Skeleton className="h-6 w-16 rounded-full" />
      </div>
      <div className="flex items-center gap-2">
        <Skeleton className="h-10 w-10 shrink-0 rounded-xl" />
        <Skeleton className="h-2 flex-1 rounded-md" />
        <Skeleton className="h-10 w-10 shrink-0 rounded-xl" />
      </div>
    </div>
  );
}

export function SwitchRowSkeleton({
  labelWidth = 'w-24',
  withIcon = false,
}: {
  labelWidth?: string;
  withIcon?: boolean;
}) {
  return (
    <div className="flex items-center justify-between gap-3">
      <div className="flex items-center gap-2">
        {withIcon && <Skeleton className="h-3.5 w-3.5 rounded-sm" />}
        <Skeleton className={`h-4 ${labelWidth}`} />
      </div>
      <Skeleton className="h-6 w-10 shrink-0 rounded-full" />
    </div>
  );
}

export function LevelToggleRowSkeleton({
  withHint = false,
}: {
  withHint?: boolean;
}) {
  return (
    <div className="flex items-center justify-between gap-3">
      <div className="flex items-center gap-1.5">
        <Skeleton className="h-4 w-28" />
        {withHint && <Skeleton className="h-4 w-4 rounded-sm" />}
      </div>
      <Skeleton className="h-8 w-[7.5rem] rounded-md" />
    </div>
  );
}

export function ImagingPreviewSkeleton() {
  return (
    <div className="relative aspect-video w-full max-w-full overflow-hidden rounded-xl bg-black md:max-w-none md:rounded-2xl">
      <Skeleton className="absolute inset-0 h-full w-full rounded-none" />
    </div>
  );
}

export function LightingControlSkeleton() {
  return (
    <Card className="bg-background shadow-sm">
      <CardContent className="space-y-5 p-5">
        <Skeleton className="h-4 w-28" />
        <SwitchRowSkeleton labelWidth="w-20" />
        <MotorAxisRowSkeleton />
        <SwitchRowSkeleton labelWidth="w-20" />
        <MotorAxisRowSkeleton />
      </CardContent>
    </Card>
  );
}

export function LensControlSkeleton() {
  return (
    <Card className="bg-background shadow-sm">
      <CardContent className="space-y-5 p-5">
        <Skeleton className="h-4 w-24" />

        <div className="flex items-center justify-between gap-4">
          <div className="flex items-center gap-1.5">
            <Skeleton className="h-4 w-24" />
            <Skeleton className="h-7 w-7 rounded-md" />
          </div>
          <div className="flex items-center gap-1">
            <Skeleton className="h-8 w-8 rounded-md" />
            <Skeleton className="h-9 w-9 rounded-md" />
          </div>
        </div>

        <MotorAxisRowSkeleton />
        <MotorAxisRowSkeleton />

        <div className="flex items-center justify-between gap-3">
          <div className="flex items-center gap-1.5">
            <Skeleton className="h-4 w-28" />
            <Skeleton className="h-7 w-7 rounded-md" />
          </div>
          <Skeleton className="h-6 w-10 shrink-0 rounded-full" />
        </div>
      </CardContent>
    </Card>
  );
}

export function DeviceOverviewSkeleton({
  compact = false,
}: {
  compact?: boolean;
}) {
  return (
    <div className={`grid grid-cols-3 gap-3 ${compact ? '' : 'sm:gap-4'}`}>
      {Array.from({ length: 3 }).map((_, i) => (
        <div
          key={i}
          className="flex items-center gap-2.5 rounded-lg border px-3 py-2.5"
        >
          <Skeleton className="h-3.5 w-3.5 shrink-0 rounded-sm" />
          <div className="min-w-0 flex-1 space-y-1.5">
            <Skeleton className="h-3 w-16" />
            <Skeleton className="h-4 w-12" />
          </div>
        </div>
      ))}
    </div>
  );
}

export function PeripheralControlSkeleton() {
  return (
    <Card className="bg-background shadow-sm">
      <CardContent className="space-y-5 p-5">
        <div className="flex items-center gap-2">
          <Skeleton className="h-4 w-4 rounded-sm" />
          <Skeleton className="h-4 w-20" />
        </div>

        <SwitchRowSkeleton labelWidth="w-20" withIcon />
        <SwitchRowSkeleton labelWidth="w-20" />
        <LevelToggleRowSkeleton withHint />
        <SwitchRowSkeleton labelWidth="w-28" />
        <SwitchRowSkeleton labelWidth="w-28" />
      </CardContent>
    </Card>
  );
}
