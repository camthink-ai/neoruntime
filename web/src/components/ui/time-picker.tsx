import * as React from 'react';
import { Clock } from 'lucide-react';
import { cn } from '@/lib/utils';
import { Button } from '@/components/ui/button';
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover';

interface TimePickerProps {
  value: string; // HH:MM:SS
  onChange: (value: string) => void;
  className?: string;
  disabled?: boolean;
}

export function TimePicker({
  value,
  onChange,
  className,
  disabled,
}: TimePickerProps) {
  const [open, setOpen] = React.useState(false);
  const parts = value.split(':');
  const [hours, minutes, seconds] = [
    parseInt(parts[0] || '0', 10),
    parseInt(parts[1] || '0', 10),
    parseInt(parts[2] || '0', 10),
  ];

  const update = (h: number, m: number, s: number) => {
    onChange(
      `${String(h).padStart(2, '0')}:${String(m).padStart(2, '0')}:${String(s).padStart(2, '0')}`
    );
  };

  const increment = (type: 'h' | 'm' | 's') => {
    if (type === 'h') update((hours + 1) % 24, minutes, seconds);
    if (type === 'm') update(hours, (minutes + 1) % 60, seconds);
    if (type === 's') update(hours, minutes, (seconds + 1) % 60);
  };

  const decrement = (type: 'h' | 'm' | 's') => {
    if (type === 'h') update((hours - 1 + 24) % 24, minutes, seconds);
    if (type === 'm') update(hours, (minutes - 1 + 60) % 60, seconds);
    if (type === 's') update(hours, minutes, (seconds - 1 + 60) % 60);
  };

  const onWheel = (e: React.WheelEvent, type: 'h' | 'm' | 's') => {
    e.preventDefault();
    if (e.deltaY < 0) increment(type);
    else decrement(type);
  };

  const display = `${String(hours).padStart(2, '0')}:${String(minutes).padStart(2, '0')}:${String(seconds).padStart(2, '0')}`;

  return (
    <Popover open={open} onOpenChange={setOpen}>
      <PopoverTrigger asChild>
        <Button
          variant="outline"
          disabled={disabled}
          className={cn(
            'justify-start text-left font-mono tabular-nums',
            !value && 'text-muted-foreground',
            className
          )}
        >
          <Clock className="h-4 w-4" />
          {display}
        </Button>
      </PopoverTrigger>
      <PopoverContent className="w-auto p-4" align="start">
        <div className="flex items-center gap-1">
          {(['h', 'm', 's'] as const).map((type, idx) => (
            <React.Fragment key={type}>
              {idx > 0 && (
                <span className="text-xl font-bold text-muted-foreground px-0.5">
                  :
                </span>
              )}
              <TimeUnitColumn
                value={type === 'h' ? hours : type === 'm' ? minutes : seconds}
                max={type === 'h' ? 23 : 59}
                onIncrement={() => increment(type)}
                onDecrement={() => decrement(type)}
                onWheel={e => onWheel(e, type)}
              />
            </React.Fragment>
          ))}
        </div>
      </PopoverContent>
    </Popover>
  );
}

function TimeUnitColumn({
  value,
  max,
  onIncrement,
  onDecrement,
  onWheel,
}: {
  value: number;
  max: number;
  onIncrement: () => void;
  onDecrement: () => void;
  onWheel: (e: React.WheelEvent) => void;
}) {
  const display = String(value).padStart(2, '0');
  const prev = String((value - 1 + max + 1) % (max + 1)).padStart(2, '0');
  const next = String((value + 1) % (max + 1)).padStart(2, '0');

  return (
    <div className="flex flex-col items-center gap-1">
      <button
        type="button"
        onClick={onIncrement}
        className="text-muted-foreground hover:text-foreground text-xs px-2 py-0.5 rounded hover:bg-muted transition-colors"
      >
        &#9650;
      </button>
      <div
        className="h-8 w-10 flex flex-col items-center justify-center text-lg font-mono tabular-nums overflow-hidden"
        onWheel={onWheel}
      >
        <span className="text-xs text-muted-foreground/50 -mt-1">{prev}</span>
        <span className="font-semibold text-foreground -mt-2">{display}</span>
        <span className="text-xs text-muted-foreground/50 -mt-2">{next}</span>
      </div>
      <button
        type="button"
        onClick={onDecrement}
        className="text-muted-foreground hover:text-foreground text-xs px-2 py-0.5 rounded hover:bg-muted transition-colors"
      >
        &#9660;
      </button>
    </div>
  );
}
