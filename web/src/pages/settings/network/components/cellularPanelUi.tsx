import type { ReactNode } from 'react';
import { cn } from '@/lib/utils';
import type { LucideIcon } from 'lucide-react';

export function SignalBars({
  rsrp,
  className,
}: {
  rsrp: number | null | undefined;
  className?: string;
}) {
  const level =    rsrp == null
      ? 0
      : rsrp >= -85
        ? 4
        : rsrp >= -95
          ? 3
          : rsrp >= -105
            ? 2
            : rsrp >= -115
              ? 1
              : 0;

  return (
    <div
      className={cn('flex items-end gap-[3px] h-[14px]', className)}
      aria-hidden
    >
      {[1, 2, 3, 4].map(i => (
        <span
          key={i}
          className={cn(
            'w-[3px] rounded-[1px] transition-colors',
            i === 1 && 'h-[4px]',
            i === 2 && 'h-[7px]',
            i === 3 && 'h-[10px]',
            i === 4 && 'h-[14px]',
            i <= level ? 'bg-foreground' : 'bg-muted-foreground/30'
          )}
        />
      ))}
    </div>
  );
}

export function StatusChip({
  ok,
  icon: Icon,
  label,
}: {
  ok: boolean;
  icon: LucideIcon;
  label: string;
}) {
  return (
    <div
      className={cn(
        'inline-flex items-center gap-1.5 rounded-md px-2 py-1 text-xs',
        ok
          ? 'bg-emerald-500/10 text-foreground'
          : 'bg-background/60 text-muted-foreground'
      )}
    >
      <Icon
        className={cn(
          'size-3.5 shrink-0',
          ok ? 'text-emerald-600 dark:text-emerald-400' : 'text-muted-foreground'
        )}
      />
      <span className="whitespace-nowrap">{label}</span>
    </div>
  );
}

export function SectionTitle({ children }: { children: ReactNode }) {
  return (
    <div className="mb-3 text-sm font-semibold text-foreground">{children}</div>
  );
}

export function Metric({
  label,
  icon: Icon,
  children,
}: {
  label: string;
  icon?: LucideIcon;
  children: ReactNode;
}) {
  return (
    <div className="min-w-0 rounded-lg bg-muted/40 px-3 py-2.5">
      <div className="mb-1.5 flex items-center gap-1.5 text-xs font-medium text-muted-foreground">
        {Icon ? <Icon className="size-3.5 shrink-0" /> : null}
        <span>{label}</span>
      </div>
      <div className="min-w-0 text-sm font-medium leading-snug text-foreground break-all">
        {children}
      </div>
    </div>
  );
}

export function DetailRow({
  label,
  value,
  icon: Icon,
}: {
  label: string;
  value?: string | null;
  icon?: LucideIcon;
}) {
  const display = value?.trim() ? value : '—';
  const hasValue = Boolean(value?.trim());
  return (
    <div className="flex items-start justify-between gap-4 border-b border-border/60 py-2.5 last:border-0">
      <div className="flex shrink-0 items-center gap-2 text-xs font-medium text-muted-foreground">
        {Icon ? <Icon className="size-3.5" /> : null}
        <span>{label}</span>
      </div>
      <div
        className={cn(
          'min-w-0 text-right text-sm font-medium leading-snug break-all',
          hasValue ? 'text-foreground' : 'text-muted-foreground'
        )}
        title={hasValue ? display : undefined}
      >
        {display}
      </div>
    </div>
  );
}

export function dash(v?: string | null) {
  return v?.trim() ? v : '—';
}

export function processStatusText(
  status:
    | {
        stub?: boolean;
        message?: string;
        connected?: boolean;
        module_present?: boolean;
        dialer_ready?: boolean;
      }
    | undefined,
  enabled: boolean,
  t: (key: string, fallback: string) => string
): { text: string; busy: boolean } | null {
  if (!enabled) return null;
  if (status?.stub) {
    return {
      text: t(
        'sys.cellular.preview_desc',
        'Dialer backend is not ready yet. Settings are stored locally for preview.'
      ),
      busy: false,
    };
  }
  const msg = status?.message?.trim() || '';
  if (status?.connected) return null;
  if (msg) {
    const busy =      /dialer running|auto-dial will start|waiting for data|re-reading|preparing|no sim detected/i.test(
        msg
      );
    return { text: msg, busy };
  }
  if (!status?.module_present) {
    return {
      text: t(
        'sys.cellular.process_wait_module',
        'Waiting for module on USB…'
      ),
      busy: true,
    };
  }
  return {
    text: t('sys.cellular.process_dialing', 'Preparing data session…'),
    busy: true,
  };
}
