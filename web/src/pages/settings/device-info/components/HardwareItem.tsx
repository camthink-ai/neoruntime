import type { ElementType } from 'react';

export interface HardwareItemProps {
  icon: ElementType;
  title: string;
  desc: string;
  action?: React.ReactNode;
}

export function HardwareItem({
  icon: Icon,
  title,
  desc,
  action,
}: HardwareItemProps) {
  return (
    <div className="flex flex-col gap-3 p-4 transition-colors hover:bg-muted/5 md:flex-row md:items-center md:justify-between md:gap-4 md:p-6">
      <div className="flex min-w-0 items-center gap-4">
        <div className="flex h-11 w-11 shrink-0 items-center justify-center rounded-lg bg-muted">
          <Icon className="h-5.5 w-5.5 text-foreground/80" />
        </div>
        <div className="flex min-w-0 flex-col gap-1">
          <div className="text-sm font-medium text-muted-foreground">
            {title}
          </div>
          <div className="wrap-break-word text-sm text-foreground">{desc}</div>
        </div>
      </div>
      {action && (
        <div className="hidden shrink-0 md:block md:pl-4">{action}</div>
      )}
    </div>
  );
}
