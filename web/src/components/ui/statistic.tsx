import { Card } from '@/components/ui/card';
import { cn } from '@/lib/utils';
import type { LucideIcon } from 'lucide-react';
import type { CSSProperties } from 'react';

interface StatisticProps {
  title: string;
  value: string | number;
  prefix?: LucideIcon;
  suffix?: string;
  valueStyle?: CSSProperties;
  className?: string;
}

export function Statistic({
  title,
  value,
  prefix: PrefixIcon,
  suffix,
  valueStyle,
  className,
}: StatisticProps) {
  return (
    <Card className={cn('p-6', className)}>
      <div className="flex flex-col gap-2">
        <div className="text-sm text-muted-foreground">{title}</div>
        <div className="flex items-center gap-2">
          {PrefixIcon && <PrefixIcon className="h-6 w-6 text-foreground" />}
          <span
            className="text-2xl font-semibold text-foreground"
            style={valueStyle}
          >
            {value}
            {suffix && <span className="text-base ml-1">{suffix}</span>}
          </span>
        </div>
      </div>
    </Card>
  );
}
