import { Skeleton } from '@/components/ui/skeleton';
import { Card, CardContent } from '@/components/ui/card';

/**
 * Skeleton for the OSD settings sidebar (text / datetime / image overlays).
 * Mirrors the section/card layout of `OsdSettings` so the loading state reads
 * as "this region is loading its config" rather than a bare spinner.
 *
 * Item rows use `border` (not a background fill) to match the live sidebar,
 * which dropped its item backgrounds in favor of a plain border outline.
 */
export function OsdSettingsSkeleton() {
  return (
    <div className="space-y-4">
      {/* Stream selector */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center gap-1.5">
            <Skeleton className="h-4 w-4 rounded-sm" />
            <Skeleton className="h-4 w-16" />
          </div>
          <div className="flex gap-2">
            {[0, 1, 2].map(i => (
              <Skeleton key={i} className="h-10 flex-1 rounded-md" />
            ))}
          </div>
          <Skeleton className="h-3 w-full" />
          <Skeleton className="h-3 w-2/3" />
        </CardContent>
      </Card>

      {/* Text overlay — header + bordered item rows (font size + text input) */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-1.5">
              <Skeleton className="h-4 w-4 rounded-sm" />
              <Skeleton className="h-4 w-20" />
            </div>
            <Skeleton className="h-7 w-14 rounded-md" />
          </div>
          {[0, 1].map(i => (
            <div
              key={i}
              className="space-y-2 rounded-md border border-border p-2"
            >
              {/* Font size row */}
              <div className="flex items-center gap-2">
                <Skeleton className="h-3 w-12" />
                <Skeleton className="h-7 w-16 rounded-md" />
              </div>
              {/* Text input + visibility + delete */}
              <div className="flex items-center gap-2">
                <Skeleton className="h-7 flex-1 rounded-md" />
                <Skeleton className="h-7 w-7 rounded-md" />
                <Skeleton className="h-7 w-7 rounded-md" />
              </div>
            </div>
          ))}
        </CardContent>
      </Card>

      {/* DateTime overlay — header + switch */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-1.5">
              <Skeleton className="h-4 w-4 rounded-sm" />
              <Skeleton className="h-4 w-24" />
            </div>
            <Skeleton className="h-6 w-10 rounded-full" />
          </div>
          <Skeleton className="h-3 w-full" />
        </CardContent>
      </Card>

      {/* Image overlay — header + upload item rows */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-1.5">
              <Skeleton className="h-4 w-4 rounded-sm" />
              <Skeleton className="h-4 w-20" />
            </div>
            <Skeleton className="h-7 w-14 rounded-md" />
          </div>
          {[0].map(i => (
            <div key={i} className="space-y-2 rounded-md p-2">
              {/* Upload button + visibility + delete */}
              <div className="flex items-center gap-2">
                <Skeleton className="h-7 flex-1 rounded-md" />
                <Skeleton className="h-7 w-7 rounded-md" />
                <Skeleton className="h-7 w-7 rounded-md" />
              </div>
            </div>
          ))}
        </CardContent>
      </Card>
    </div>
  );
}

/**
 * Skeleton for the privacy-mask sidebar.
 *
 * Mirrors the 2-card layout of `PrivacyMaskSettings`: the static mask card
 * (master toggle + style + regions as nested sub-sections) and the independent
 * DPM card (AI labels + mode). Regions are nested inside the mask card, not a
 * separate card.
 */
export function PrivacyMaskSkeleton() {
  return (
    <div className="space-y-4">
      {/* Privacy mask — master toggle + style + regions in one card */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-1.5">
              <Skeleton className="h-4 w-4 rounded-sm" />
              <Skeleton className="h-4 w-20" />
            </div>
            <Skeleton className="h-6 w-10 rounded-full" />
          </div>
          <Skeleton className="h-3 w-full" />

          {/* Style sub-section */}
          <div className="space-y-2 border-t border-border pt-3">
            <Skeleton className="h-3 w-16" />
            <Skeleton className="h-5 w-full rounded-full" />
          </div>

          {/* Regions sub-section */}
          <div className="space-y-2 border-t border-border pt-3">
            <div className="flex items-center justify-between">
              <Skeleton className="h-3 w-16" />
              <Skeleton className="h-7 w-24 rounded-md" />
            </div>
            <Skeleton className="h-3 w-full" />
            <div className="flex items-center justify-between rounded-md border border-border p-2">
              <div className="flex items-center gap-2">
                <Skeleton className="h-5 w-9 rounded-full" />
                <Skeleton className="h-3 w-20" />
              </div>
              <Skeleton className="h-6 w-6 rounded-md" />
            </div>
          </div>
        </CardContent>
      </Card>

      {/* Dynamic privacy mask (DPM) — labels + mode */}
      <Card className="bg-background shadow-sm">
        <CardContent className="space-y-3 p-4">
          <div className="flex items-center justify-between">
            <div className="flex items-center gap-1.5">
              <Skeleton className="h-4 w-4 rounded-sm" />
              <Skeleton className="h-4 w-24" />
            </div>
            <Skeleton className="h-6 w-10 rounded-full" />
          </div>
          <Skeleton className="h-3 w-full" />

          {/* DPM target groups (segmented controls) */}
          {[0, 1].map(g => (
            <div key={g} className="space-y-1.5">
              <Skeleton className="h-3 w-14" />
              <div className="grid grid-cols-2 overflow-hidden rounded-md border border-border">
                <Skeleton className="h-9 rounded-none" />
                <Skeleton className="h-9 rounded-none border-l border-border" />
              </div>
            </div>
          ))}

          {/* DPM mode select */}
          <div className="space-y-1.5">
            <Skeleton className="h-3 w-16" />
            <Skeleton className="h-9 w-full rounded-md" />
          </div>
        </CardContent>
      </Card>
    </div>
  );
}
