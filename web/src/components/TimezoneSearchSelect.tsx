import { useMemo, useRef, useState } from 'react';
import type { TFunction } from 'i18next';

import { Check, ChevronDown, Search } from 'lucide-react';
import {
  Popover,
  PopoverContent,
  PopoverTrigger,
} from '@/components/ui/popover';
import { cn } from '@/lib/utils';
import { translateTimezoneCountry } from '@/pages/settings/time/utils/timezoneCountry';

export interface TimezoneItem {
  name: string;
  country: string;
  offset: string;
}

interface TimezoneSearchSelectProps {
  timezones: TimezoneItem[];
  value: string;
  onChange: (value: string) => void;
  placeholder: string;
  t: TFunction;
}

export function TimezoneSearchSelect({
  timezones,
  value,
  onChange,
  placeholder,
  t,
}: TimezoneSearchSelectProps) {
  const [open, setOpen] = useState(false);
  const [search, setSearch] = useState('');
  const inputRef = useRef<HTMLInputElement>(null);

  const filtered = useMemo(() => {
    if (!search) return timezones;

    const query = search.toLowerCase();

    return timezones.filter(
      timezone => timezone.name.toLowerCase().includes(query)
        || timezone.country.toLowerCase().includes(query)
        || timezone.offset.toLowerCase().includes(query)
    );
  }, [search, timezones]);

  const selected = timezones.find(timezone => timezone.name === value);

  return (
    <Popover
      open={open}
      onOpenChange={nextOpen => {
        setOpen(nextOpen);

        if (nextOpen) {
          setSearch('');
          setTimeout(() => inputRef.current?.focus(), 0);
        }
      }}
    >
      <PopoverTrigger asChild>
        <button
          type="button"
          className="flex h-9 max-w-md w-full items-center justify-between rounded-md border border-input bg-transparent px-3 py-1 text-sm shadow-xs transition-colors  hover:text-accent-foreground focus-visible:outline-none focus-visible:ring-1 focus-visible:ring-ring"
        >
          <span
            className={selected ? 'text-foreground' : 'text-muted-foreground'}
          >
            {selected
              ? `${selected.name} (${selected.offset}, ${translateTimezoneCountry(selected.country, t)})`
              : placeholder}
          </span>
          <ChevronDown className="ml-2 h-4 w-4 shrink-0 opacity-50" />
        </button>
      </PopoverTrigger>
      <PopoverContent
        className="w-(--radix-popover-trigger-width) max-w-md p-0"
        align="start"
      >
        <div className="flex items-center border-b px-3">
          <Search className="h-4 w-4 shrink-0 opacity-50" />
          <input
            ref={inputRef}
            value={search}
            onChange={event => setSearch(event.target.value)}
            placeholder={placeholder}
            className="flex h-10 w-full bg-transparent py-3 text-sm outline-none placeholder:text-muted-foreground"
          />
        </div>
        <div className="max-h-64 overflow-y-auto p-1">
          {filtered.length === 0 && (
            <div className="py-6 text-center text-sm text-muted-foreground">
              {t('sys.time.no_match', '无匹配结果')}
            </div>
          )}
          {filtered.map(timezone => (
            <button
              key={timezone.name}
              type="button"
              onClick={() => {
                onChange(timezone.name);
                setOpen(false);
              }}
              className={cn(
                'relative flex w-full cursor-pointer select-none items-center rounded-sm px-2 py-1.5 text-sm outline-none',
                'hover:bg-accent hover:text-accent-foreground',
                timezone.name === value && 'bg-accent'
              )}
            >
              <span className="flex-1 text-left">{timezone.name}</span>
              <span className="ml-2 text-xs text-muted-foreground">
                {translateTimezoneCountry(timezone.country, t)}
              </span>
              <span className="ml-2 text-xs tabular-nums text-muted-foreground">
                {timezone.offset}
              </span>
              {timezone.name === value && (
                <Check className="ml-2 h-4 w-4 shrink-0 text-primary" />
              )}
            </button>
          ))}
        </div>
      </PopoverContent>
    </Popover>
  );
}
