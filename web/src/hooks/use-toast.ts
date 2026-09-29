import { toast as sonnerToast } from 'sonner';

interface ToastOptions {
  title: string;
  description?: string;
  variant?: 'default' | 'destructive' | 'warning';
}

export function useToast() {
  const toast = ({ title, description, variant }: ToastOptions) => {
    if (variant === 'destructive') {
      sonnerToast.error(title, {
        description,
      });
    } else if (variant === 'warning') {
      // Partial success: the operation committed but a follow-up step
      // failed (e.g. model updated but not reloaded on the NPU).
      sonnerToast.warning(title, {
        description,
      });
    } else {
      sonnerToast.success(title, {
        description,
      });
    }
  };

  return { toast };
}
