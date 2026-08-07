import type { CSSProperties } from 'react';
import { cn } from '@/lib/utils';

interface SvgIconProps {
  icon: string;
  prefix?: string;
  color?: string;
  className?: string;
  style?: CSSProperties;
}

export default function SvgIcon({
  icon,
  prefix = 'icon',
  color = 'currentColor',
  className = '',
  style = {},
  ...props
}: SvgIconProps) {
  const symbolId = `#${prefix}-${icon}`;

  return (
    <svg
      {...props}
      className={cn('w-full h-full', className)}
      style={style}
      aria-hidden="true"
      focusable="false"
    >
      <use href={symbolId} xlinkHref={symbolId} fill={color} />
    </svg>
  );
}
