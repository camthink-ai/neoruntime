import { useTranslation } from 'react-i18next';
import {
  AlertDialog,
  AlertDialogAction,
  AlertDialogCancel,
  AlertDialogContent,
  AlertDialogDescription,
  AlertDialogFooter,
  AlertDialogHeader,
  AlertDialogTitle,
} from '@/components/ui/alert-dialog';

interface ProcessInfo {
  pid: number;
  name: string;
  cmdline: string;
}

interface KillProcessDialogProps {
  open: boolean;
  onOpenChange: (open: boolean) => void;
  process: ProcessInfo | null;
  onConfirm: () => void;
}

export function KillProcessDialog({
  open,
  onOpenChange,
  process,
  onConfirm,
}: KillProcessDialogProps) {
  const { t } = useTranslation();

  return (
    <AlertDialog open={open} onOpenChange={onOpenChange}>
      <AlertDialogContent>
        <AlertDialogHeader>
          <AlertDialogTitle>
            {t('maintenance.processes.confirm_kill', 'Terminate Process?')}
          </AlertDialogTitle>
          <AlertDialogDescription>
            {t(
              'maintenance.processes.kill_warning',
              'This will send SIGTERM to the process, allowing it to perform cleanup before exiting.'
            )}
            <div className="mt-3 rounded-md bg-muted p-3 text-sm">
              <div className="font-semibold">
                {process?.name} (PID: {process?.pid})
              </div>
              <div className="text-muted-foreground mt-1 font-mono text-xs truncate">
                {process?.cmdline}
              </div>
            </div>
          </AlertDialogDescription>
        </AlertDialogHeader>
        <AlertDialogFooter>
          <AlertDialogCancel>{t('common.cancel', 'Cancel')}</AlertDialogCancel>
          <AlertDialogAction
            onClick={onConfirm}
            className="bg-destructive hover:bg-destructive/90"
          >
            {t('maintenance.processes.terminate', 'Terminate')}
          </AlertDialogAction>
        </AlertDialogFooter>
      </AlertDialogContent>
    </AlertDialog>
  );
}
