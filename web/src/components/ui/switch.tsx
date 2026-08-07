import * as React from 'react';

import { cn } from '@/lib/utils';

export interface SwitchProps extends Omit<
  React.ButtonHTMLAttributes<HTMLButtonElement>,
  'onChange'
> {
  checked: boolean;
  onCheckedChange: (checked: boolean) => void;
  loading?: boolean;
}

export function Switch({
  checked,
  onCheckedChange,
  loading = false,
  className,
  disabled,
  ...props
}: SwitchProps) {
  const isDisabled = disabled || loading;

  return (
    <button
      type="button"
      role="switch"
      aria-checked={checked}
      aria-busy={loading || undefined}
      data-state={checked ? 'checked' : 'unchecked'}
      data-loading={loading ? 'true' : 'false'}
      disabled={isDisabled}
      onClick={() => !isDisabled && onCheckedChange(!checked)}
      onKeyDown={e => {
        if (isDisabled) return;
        if (e.key === 'Enter' || e.key === ' ') {
          e.preventDefault();
          onCheckedChange(!checked);
        }
      }}
      className={cn(
        'relative inline-flex h-6 w-11 shrink-0 cursor-pointer items-center rounded-full border-2 border-transparent transition-colors',
        checked ? 'bg-primary' : 'bg-input',
        isDisabled && 'cursor-not-allowed opacity-60',
        className
      )}
      {...props}
    >
      <span
        aria-hidden
        className={cn(
          'pointer-events-none inline-flex h-5 w-5 transform items-center justify-center rounded-full bg-switch-thumb shadow transition-transform',
          checked ? 'translate-x-5' : 'translate-x-0',
          loading && 'opacity-0'
        )}
      />
      {loading && (
        <span
          aria-hidden
          className={cn(
            'pointer-events-none absolute flex h-5 w-5 items-center justify-center',
            checked ? 'right-0.5' : 'left-0.5'
          )}
        >
          <span className="h-3.5 w-3.5 animate-spin rounded-full border-2 border-foreground/20 border-t-foreground/70" />
        </span>
      )}
    </button>
  );
}
