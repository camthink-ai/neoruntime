import { useEffect, useMemo, createContext, useContext } from 'react';
import { createPortal } from 'react-dom';
import { cn } from '@/lib/utils';
import { Button } from './button';

interface AlertDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  children: React.ReactNode;
}

interface AlertDialogContextValue {
  onOpenChange: (open: boolean) => void;
}

const AlertDialogContext = createContext<AlertDialogContextValue | undefined>(
  undefined
);

const useAlertDialog = () => {
  const context = useContext(AlertDialogContext);
  if (!context) {
    throw new Error('AlertDialog components must be used within AlertDialog');
  }
  return context;
};

export function AlertDialog({
  open,
  onOpenChange,
  children,
}: AlertDialogProps) {
  const contextValue = useMemo(() => ({ onOpenChange }), [onOpenChange]);

  useEffect(() => {
    if (open) {
      document.body.style.overflow = 'hidden';
    } else {
      document.body.style.overflow = '';
    }
    return () => {
      document.body.style.overflow = '';
    };
  }, [open]);

  if (!open) return null;

  return createPortal(
    <AlertDialogContext.Provider value={contextValue}>
      <div
        className="fixed inset-0 flex items-center justify-center"
        style={{ zIndex: 9999, pointerEvents: 'auto' }}
        onClick={() => onOpenChange(false)}
      >
        <div className="absolute inset-0 bg-black/50" />
        <div className="relative z-10" onClick={e => e.stopPropagation()}>
          {children}
        </div>
      </div>
    </AlertDialogContext.Provider>,
    document.body
  );
}

export function AlertDialogContent({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <div
      className={cn(
        'bg-background border border-border rounded-lg shadow-lg p-6 max-w-md w-full mx-4 max-h-[85vh] overflow-y-auto',
        className
      )}
    >
      {children}
    </div>
  );
}

export function AlertDialogHeader({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return <div className={cn('space-y-2 mb-4', className)}>{children}</div>;
}

export function AlertDialogTitle({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return <h2 className={cn('text-lg font-semibold', className)}>{children}</h2>;
}

export function AlertDialogDescription({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <p className={cn('text-sm text-muted-foreground', className)}>{children}</p>
  );
}

export function AlertDialogFooter({
  children,
  className,
}: {
  children: React.ReactNode;
  className?: string;
}) {
  return (
    <div className={cn('flex justify-end space-x-2 mt-6', className)}>
      {children}
    </div>
  );
}

export function AlertDialogCancel({
  children,
  onClick,
  ...props
}: React.ComponentProps<typeof Button>) {
  const { onOpenChange } = useAlertDialog();

  const handleClick = (e: React.MouseEvent<HTMLButtonElement>) => {
    onOpenChange(false);
    onClick?.(e);
  };

  return (
    <Button variant="outline" onClick={handleClick} {...props}>
      {children}
    </Button>
  );
}

export function AlertDialogAction({
  children,
  ...props
}: React.ComponentProps<typeof Button>) {
  return <Button {...props}>{children}</Button>;
}
