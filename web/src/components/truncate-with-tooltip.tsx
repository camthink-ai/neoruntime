import { useState, useRef, useEffect, type ReactNode } from 'react';
import {
  Tooltip,
  TooltipContent,
  TooltipTrigger,
} from '@/components/ui/tooltip';

export interface TruncateWithTooltipProps {
  value: string;
  className?: string;
  tooltipClassName?: string;
  tooltipContentClassName?: string;
  children?: ReactNode;
}

/**
 * 文本在容器内被截断（overflow）时才显示 Tooltip，避免无意义的气泡。
 */
export function TruncateWithTooltip({
  value,
  className = '',
  tooltipClassName = '',
  tooltipContentClassName = '',
  children,
}: TruncateWithTooltipProps) {
  const ref = useRef<HTMLDivElement>(null);
  const [isOverflowing, setIsOverflowing] = useState(false);

  useEffect(() => {
    const checkOverflow = () => {
      if (ref.current) {
        setIsOverflowing(ref.current.scrollWidth > ref.current.clientWidth);
      }
    };

    checkOverflow();

    const resizeObserver = new ResizeObserver(checkOverflow);
    if (ref.current) {
      resizeObserver.observe(ref.current);
    }

    return () => resizeObserver.disconnect();
  }, [value]);

  const content = children || <span>{value}</span>;

  if (isOverflowing) {
    return (
      <Tooltip>
        <TooltipTrigger asChild>
          <div ref={ref} className={`truncate ${className}`}>
            {content}
          </div>
        </TooltipTrigger>
        <TooltipContent side="bottom" className={tooltipClassName}>
          <span className={`font-mono ${tooltipContentClassName}`}>
            {value}
          </span>
        </TooltipContent>
      </Tooltip>
    );
  }

  return (
    <div ref={ref} className={`truncate ${className}`}>
      {content}
    </div>
  );
}
