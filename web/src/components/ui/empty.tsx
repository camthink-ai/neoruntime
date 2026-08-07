import { cn } from '@/lib/utils';
import type { LucideIcon } from 'lucide-react';

interface EmptyProps {
  description?: string;
  icon?: LucideIcon;
  className?: string;
}

export function Empty({
  description = 'No data',
  icon: Icon,
  className,
}: EmptyProps) {
  return (
    <div
      className={cn(
        'flex flex-col items-center justify-center py-12 text-muted-foreground',
        className
      )}
    >
      {Icon && <Icon className="h-16 w-16 mb-4 opacity-40" />}
      <p className="text-sm">{description}</p>
    </div>
  );
}
