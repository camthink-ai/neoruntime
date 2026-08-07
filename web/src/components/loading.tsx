import { createPortal } from 'react-dom';
import { cn } from '@/lib/utils';
import SvgIcon from '@/components/svg-icon';

interface LoadingProps {
  /** Show a full-screen mask overlay */
  isMask?: boolean;
  /** Custom class name for the container */
  className?: string;
  /** Center the loading in a full-height container */
  fullHeight?: boolean;
  /** Size of the loading icon */
  size?: 'sm' | 'md' | 'lg' | string;
  /** Placeholder text to display instead of default text */
  placeholder?: string;
}

export default function Loading({
  isMask = false,
  className,
  fullHeight = true,
  size = 'md',
  placeholder,
}: LoadingProps) {
  const iconSizeClasses = {
    sm: 'w-8 h-8',
    md: 'w-12 h-12',
    lg: 'w-16 h-16',
  };

  const getIconClass = () => (iconSizeClasses as Record<string, string>)[size] || iconSizeClasses.md;

  const content = (
    <div
      className={cn(
        'w-full flex flex-col items-center justify-center overflow-hidden',
        fullHeight && 'h-full min-h-[120px]',
        className
      )}
    >
      <SvgIcon
        icon="ipc"
        className={cn(
          getIconClass(),
          'text-foreground/80 drop-shadow-sm animate-pulse'
        )}
      />
      <span className="text-sm mt-2 text-muted-foreground flex items-center justify-center">
        {placeholder || 'loading'}
        {!placeholder && (
          <span className="inline-flex w-4 overflow-hidden ml-0.5">
            <span className="animate-[dotBounce_1.4s_ease-in-out_infinite]">
              .
            </span>
            <span className="animate-[dotBounce_1.4s_ease-in-out_0.2s_infinite]">
              .
            </span>
            <span className="animate-[dotBounce_1.4s_ease-in-out_0.4s_infinite]">
              .
            </span>
          </span>
        )}
      </span>
    </div>
  );

  if (isMask && typeof document !== 'undefined') {
    return createPortal(
      <div className="fixed inset-0 bg-black/50 z-10000 pointer-events-auto flex items-center justify-center">
        {content}
      </div>,
      document.body
    );
  }

  return content;
}
